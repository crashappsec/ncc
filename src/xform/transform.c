// transform.c — Tree transform framework for slay parse trees.
//
// Provides:
//  - Registry-based pre/post-order tree transformer
//  - Tree mutation primitives (set/remove/insert child, clone)
//  - Node construction helpers (make NT, make token, make with children)
//  - Stack-based iterative tree walker
//  - Template-based subtree construction (parse a snippet as a subtree)

#include "xform/transform.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: NT id lookup by name
// ============================================================================

static int64_t
nt_id_for_name(ncc_grammar_t *grammar, const char *nt_name)
{
    if (!grammar || !nt_name) {
        return -1;
    }

    ncc_string_t s = ncc_string_from_cstr(nt_name);
    ncc_nonterm_t *nt = ncc_nonterm(grammar, s);

    if (!nt) {
        return -1;
    }

    return ncc_nonterm_id(nt);
}

// ============================================================================
// Parent pointer helper
// ============================================================================

static inline void
set_node_parent(ncc_parse_tree_t *child, ncc_parse_tree_t *parent)
{
    if (child && !ncc_tree_is_leaf(child)) {
        ncc_nt_node_t pn = ncc_tree_node_value(child);
        pn.parent = parent;
        child->node.value = pn;
    }
}

static void
set_parent_pointers_recursive(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    size_t nch = ncc_tree_num_children(node);
    for (size_t i = 0; i < nch; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (child) {
            set_node_parent(child, node);
            set_parent_pointers_recursive(child);
        }
    }
}

void
ncc_xform_set_parent_pointers(ncc_parse_tree_t *tree)
{
    set_parent_pointers_recursive(tree);
}

// ============================================================================
// Tree mutation primitives
// ============================================================================

bool
ncc_xform_set_child(ncc_parse_tree_t *parent, size_t index,
                      ncc_parse_tree_t *new_child)
{
    bool ok = ncc_tree_set_child(parent, index, new_child);
    if (ok) {
        set_node_parent(new_child, parent);
    }
    return ok;
}

ncc_parse_tree_t *
ncc_xform_remove_child(ncc_parse_tree_t *parent, size_t index)
{
    ncc_parse_tree_t *removed = ncc_tree_remove_child(parent, index);
    if (removed) {
        set_node_parent(removed, nullptr);
    }
    return removed;
}

bool
ncc_xform_insert_child(ncc_parse_tree_t *parent, size_t index,
                          ncc_parse_tree_t *child)
{
    bool ok = ncc_tree_insert_child_at(parent, index, child);
    if (ok) {
        set_node_parent(child, parent);
    }
    return ok;
}

// Deep-copy a trivia linked list.
static ncc_trivia_t *
clone_trivia_chain(ncc_trivia_t *src)
{
    ncc_trivia_t *head = nullptr;
    ncc_trivia_t *tail = nullptr;

    for (ncc_trivia_t *t = src; t; t = t->next) {
        ncc_trivia_t *copy = ncc_alloc(ncc_trivia_t);

        if (!copy) {
            break;
        }

        // Deep-copy the text string so it's independently owned.
        if (t->text.data && t->text.u8_bytes > 0) {
            copy->text = ncc_string_from_raw(t->text.data,
                                               (int64_t)t->text.u8_bytes);
        }
        else {
            copy->text = (ncc_string_t){0};
        }

        copy->next = nullptr;

        if (!head) {
            head = copy;
        }
        else {
            tail->next = copy;
        }

        tail = copy;
    }

    return head;
}

ncc_parse_tree_t *
ncc_xform_clone(ncc_parse_tree_t *tree)
{
    if (!tree) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(tree)) {
        ncc_token_info_t *orig_tok = ncc_tree_leaf_value(tree);

        ncc_token_info_t *tok_copy = ncc_alloc(ncc_token_info_t);

        if (tok_copy && orig_tok) {
            *tok_copy = *orig_tok;

            // Deep-copy string value so the clone is independent.
            if (ncc_option_is_set(orig_tok->value)) {
                ncc_string_t val = ncc_option_get(orig_tok->value);

                if (val.data && val.u8_bytes > 0) {
                    ncc_string_t val_copy = ncc_string_from_raw(
                        val.data, (int64_t)val.u8_bytes);
                    tok_copy->value = ncc_option_set(ncc_string_t, val_copy);
                }
            }

            // Deep-copy trivia chains so they survive source cleanup.
            tok_copy->leading_trivia  = clone_trivia_chain(
                orig_tok->leading_trivia);
            tok_copy->trailing_trivia = clone_trivia_chain(
                orig_tok->trailing_trivia);
        }

        return ncc_tree_leaf(ncc_nt_node_t, ncc_token_info_ptr_t, tok_copy);
    }

    ncc_nt_node_t pn = ncc_tree_node_value(tree);
    pn.parent = nullptr;

    ncc_parse_tree_t *copy = ncc_tree_node(ncc_nt_node_t,
                                               ncc_token_info_ptr_t, pn);

    size_t nch = ncc_tree_num_children(tree);

    for (size_t i = 0; i < nch; i++) {
        ncc_parse_tree_t *child_copy = ncc_xform_clone(
            ncc_tree_child(tree, i));
        ncc_tree_add_child(copy, child_copy);
        set_node_parent(child_copy, copy);
    }

    return copy;
}

// ============================================================================
// Node construction helpers
// ============================================================================

ncc_parse_tree_t *
ncc_xform_make_nt_node(ncc_grammar_t *grammar, const char *nt_name,
                          int32_t rule_index)
{
    ncc_nt_node_t pn = {0};

    if (nt_name) {
        pn.name = ncc_string_from_cstr(nt_name);
    }

    pn.rule_index = rule_index;
    pn.id         = nt_id_for_name(grammar, nt_name);

    return ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
}

ncc_parse_tree_t *
ncc_xform_make_token_node(int64_t tid, const char *value,
                             uint32_t line, uint32_t col)
{
    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);

    if (!tok) {
        return nullptr;
    }

    memset(tok, 0, sizeof(*tok));
    tok->tid    = (int32_t)tid;
    tok->line   = line;
    tok->column = col;

    if (value) {
        ncc_string_t s = ncc_string_from_cstr(value);
        tok->value = ncc_option_set(ncc_string_t, s);
    }
    else {
        tok->value = ncc_option_none(ncc_string_t);
    }

    return ncc_tree_leaf(ncc_nt_node_t, ncc_token_info_ptr_t, tok);
}

ncc_parse_tree_t *
ncc_xform_make_node_with_children(ncc_grammar_t     *grammar,
                                     const char         *nt_name,
                                     int32_t             rule_index,
                                     ncc_parse_tree_t **children,
                                     size_t              count)
{
    ncc_parse_tree_t *node = ncc_xform_make_nt_node(grammar, nt_name,
                                                        rule_index);
    if (!node) {
        return nullptr;
    }

    for (size_t i = 0; i < count; i++) {
        if (children[i]) {
            ncc_tree_add_child(node, children[i]);
            set_node_parent(children[i], node);
        }
    }

    return node;
}

// ============================================================================
// Registry
// ============================================================================

void
ncc_xform_registry_init(ncc_xform_registry_t *reg, ncc_grammar_t *grammar)
{
    assert(reg);
    memset(reg, 0, sizeof(*reg));
    reg->grammar = grammar;
}

void
ncc_xform_registry_free(ncc_xform_registry_t *reg)
{
    if (!reg) {
        return;
    }

    for (int i = 0; i < NCC_XFORM_MAX_DIRECT_NTS; i++) {
        if (reg->pre_order[i]) {
            for (int j = 0; j < reg->pre_count[i]; j++) {
                ncc_free(reg->pre_order[i][j]);
            }

            ncc_free(reg->pre_order[i]);
        }

        if (reg->post_order[i]) {
            for (int j = 0; j < reg->post_count[i]; j++) {
                ncc_free(reg->post_order[i][j]);
            }

            ncc_free(reg->post_order[i]);
        }
    }

    if (reg->wildcard_pre) {
        for (int i = 0; i < reg->wildcard_pre_count; i++) {
            ncc_free(reg->wildcard_pre[i]);
        }

        ncc_free(reg->wildcard_pre);
    }

    if (reg->wildcard_post) {
        for (int i = 0; i < reg->wildcard_post_count; i++) {
            ncc_free(reg->wildcard_post[i]);
        }

        ncc_free(reg->wildcard_post);
    }

    memset(reg, 0, sizeof(*reg));
}

// Internal: add an entry to a dynamic array.
static bool
add_entry(ncc_xform_entry_t ***array, int *count, int *cap,
          ncc_xform_entry_t   *entry)
{
    if (*count >= *cap) {
        int new_cap = *cap ? *cap * 2 : 4;
        ncc_xform_entry_t **new_arr = ncc_alloc_size(new_cap,
                                                         sizeof(ncc_xform_entry_t *));

        if (!new_arr) {
            return false;
        }

        if (*array && *count > 0) {
            memcpy(new_arr, *array,
                   (size_t)*count * sizeof(ncc_xform_entry_t *));
        }

        if (*array) {
            ncc_free(*array);
        }

        *array = new_arr;
        *cap   = new_cap;
    }

    (*array)[(*count)++] = entry;
    return true;
}

bool
ncc_xform_register(ncc_xform_registry_t *reg, const char *nt_name,
                      ncc_xform_fn_t fn, const char *name)
{
    assert(reg && fn);

    ncc_xform_entry_t *entry = ncc_alloc(ncc_xform_entry_t);

    if (!entry) {
        return false;
    }

    entry->post_fn = fn;
    entry->name    = name;
    entry->is_pre  = false;

    if (!nt_name) {
        return add_entry(&reg->wildcard_post,
                         &reg->wildcard_post_count,
                         &reg->wildcard_post_cap, entry);
    }

    int64_t id = nt_id_for_name(reg->grammar, nt_name);

    if (id >= 0 && id < NCC_XFORM_MAX_DIRECT_NTS) {
        return add_entry(&reg->post_order[id],
                         &reg->post_count[id],
                         &reg->post_cap[id], entry);
    }

    return add_entry(&reg->wildcard_post,
                     &reg->wildcard_post_count,
                     &reg->wildcard_post_cap, entry);
}

bool
ncc_xform_register_pre(ncc_xform_registry_t *reg, const char *nt_name,
                          ncc_xform_pre_fn_t fn, const char *name)
{
    assert(reg && fn);

    ncc_xform_entry_t *entry = ncc_alloc(ncc_xform_entry_t);

    if (!entry) {
        return false;
    }

    entry->pre_fn = fn;
    entry->name   = name;
    entry->is_pre = true;

    if (!nt_name) {
        return add_entry(&reg->wildcard_pre,
                         &reg->wildcard_pre_count,
                         &reg->wildcard_pre_cap, entry);
    }

    int64_t id = nt_id_for_name(reg->grammar, nt_name);

    if (id >= 0 && id < NCC_XFORM_MAX_DIRECT_NTS) {
        return add_entry(&reg->pre_order[id],
                         &reg->pre_count[id],
                         &reg->pre_cap[id], entry);
    }

    return add_entry(&reg->wildcard_pre,
                     &reg->wildcard_pre_count,
                     &reg->wildcard_pre_cap, entry);
}

bool
ncc_xform_register_by_id(ncc_xform_registry_t *reg, int64_t nt_id,
                            ncc_xform_fn_t fn, const char *name)
{
    assert(reg && fn);

    ncc_xform_entry_t *entry = ncc_alloc(ncc_xform_entry_t);

    if (!entry) {
        return false;
    }

    entry->post_fn = fn;
    entry->name    = name;
    entry->is_pre  = false;

    if (nt_id >= 0 && nt_id < NCC_XFORM_MAX_DIRECT_NTS) {
        return add_entry(&reg->post_order[nt_id],
                         &reg->post_count[nt_id],
                         &reg->post_cap[nt_id], entry);
    }

    return add_entry(&reg->wildcard_post,
                     &reg->wildcard_post_count,
                     &reg->wildcard_post_cap, entry);
}

bool
ncc_xform_register_pre_by_id(ncc_xform_registry_t *reg, int64_t nt_id,
                                ncc_xform_pre_fn_t fn, const char *name)
{
    assert(reg && fn);

    ncc_xform_entry_t *entry = ncc_alloc(ncc_xform_entry_t);

    if (!entry) {
        return false;
    }

    entry->pre_fn = fn;
    entry->name   = name;
    entry->is_pre = true;

    if (nt_id >= 0 && nt_id < NCC_XFORM_MAX_DIRECT_NTS) {
        return add_entry(&reg->pre_order[nt_id],
                         &reg->pre_count[nt_id],
                         &reg->pre_cap[nt_id], entry);
    }

    return add_entry(&reg->wildcard_pre,
                     &reg->wildcard_pre_count,
                     &reg->wildcard_pre_cap, entry);
}

// ============================================================================
// Context
// ============================================================================

void
ncc_xform_ctx_init(ncc_xform_ctx_t      *ctx,
                     ncc_grammar_t         *grammar,
                     ncc_xform_registry_t  *reg,
                     ncc_parse_tree_t      *root)
{
    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->grammar  = grammar;
    ctx->registry = reg;
    ctx->root     = root;
}

// ============================================================================
// Internal: check if a node originates from a system header
// ============================================================================

static bool
node_in_system_header(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        return tok && tok->system_header;
    }

    // Group nodes (from BNF +, *, ?) can span both system and user code.
    // Don't skip them — let the walker descend and check each child.
    ncc_nt_node_t pn = ncc_tree_node_value(node);
    if (pn.group_top || pn.group_item) {
        return false;
    }

    // Walk to the first leaf descendant.
    size_t nch = ncc_tree_num_children(node);

    for (size_t i = 0; i < nch; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);

        if (c) {
            return node_in_system_header(c);
        }
    }

    return false;
}

// ============================================================================
// Internal: apply pre/post transformers to a single node
// ============================================================================

static ncc_parse_tree_t *
apply_pre(ncc_xform_registry_t *reg, ncc_xform_ctx_t *ctx,
          ncc_parse_tree_t *node, ncc_xform_control_t *control)
{
    *control = NCC_XFORM_CONTINUE;
    ncc_parse_tree_t *result = nullptr;

    int64_t id = ncc_pt_nt_id(node);

    // Type-specific pre transforms.
    if (id >= 0 && id < NCC_XFORM_MAX_DIRECT_NTS
        && reg->pre_count[id] > 0) {
        for (int i = 0; i < reg->pre_count[id]; i++) {
            ncc_xform_entry_t  *entry = reg->pre_order[id][i];
            ncc_xform_control_t ctrl  = NCC_XFORM_CONTINUE;
            ncc_parse_tree_t *replaced = entry->pre_fn(
                ctx, result ? result : node, &ctrl);

            if (replaced) {
                result = replaced;
                ctx->nodes_replaced++;
            }

            if (ctrl == NCC_XFORM_SKIP_CHILDREN) {
                *control = NCC_XFORM_SKIP_CHILDREN;
            }
        }
    }

    // Wildcard pre transforms.
    for (int i = 0; i < reg->wildcard_pre_count; i++) {
        ncc_xform_entry_t  *entry = reg->wildcard_pre[i];
        ncc_xform_control_t ctrl  = NCC_XFORM_CONTINUE;
        ncc_parse_tree_t *replaced = entry->pre_fn(
            ctx, result ? result : node, &ctrl);

        if (replaced) {
            result = replaced;
            ctx->nodes_replaced++;
        }

        if (ctrl == NCC_XFORM_SKIP_CHILDREN) {
            *control = NCC_XFORM_SKIP_CHILDREN;
        }
    }

    return result;
}

static ncc_parse_tree_t *
apply_post(ncc_xform_registry_t *reg, ncc_xform_ctx_t *ctx,
           ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *result = nullptr;

    int64_t id = ncc_pt_nt_id(node);

    // Type-specific post transforms.
    if (id >= 0 && id < NCC_XFORM_MAX_DIRECT_NTS
        && reg->post_count[id] > 0) {
        for (int i = 0; i < reg->post_count[id]; i++) {
            ncc_xform_entry_t *entry = reg->post_order[id][i];
            ncc_parse_tree_t *replaced = entry->post_fn(
                ctx, result ? result : node);

            if (replaced) {
                result = replaced;
                ctx->nodes_replaced++;
            }
        }
    }

    // Wildcard post transforms.
    for (int i = 0; i < reg->wildcard_post_count; i++) {
        ncc_xform_entry_t *entry = reg->wildcard_post[i];
        ncc_parse_tree_t *replaced = entry->post_fn(
            ctx, result ? result : node);

        if (replaced) {
            result = replaced;
            ctx->nodes_replaced++;
        }
    }

    return result;
}

// ============================================================================
// Tree walker (iterative stack-based)
// ============================================================================

typedef struct {
    ncc_parse_tree_t *node;
    ncc_parse_tree_t *current;  // After pre-transform.
    int                child_idx;
    bool               skip_children;
} walk_item_t;

#define WALK_STACK_INITIAL 256

static ncc_parse_tree_t *
walk_tree(ncc_xform_registry_t *reg, ncc_xform_ctx_t *ctx,
          ncc_parse_tree_t *root)
{
    if (!root) {
        return nullptr;
    }

    int          stack_cap = WALK_STACK_INITIAL;
    int          stack_top = 0;
    walk_item_t *stack     = ncc_alloc_array(walk_item_t, stack_cap);

    if (!stack) {
        return root;
    }

    stack[stack_top++] = (walk_item_t){
        .node          = root,
        .current       = root,
        .child_idx     = -1,
        .skip_children = false,
    };

    ncc_parse_tree_t *final_result = root;

    while (stack_top > 0) {
        walk_item_t *item = &stack[stack_top - 1];

        // Phase 1: Pre-order.
        if (item->child_idx == -1) {
            ncc_parse_tree_t *node = item->node;
            ctx->nodes_visited++;
            ctx->depth++;

            ncc_xform_control_t control    = NCC_XFORM_CONTINUE;
            ncc_parse_tree_t   *pre_result = apply_pre(reg, ctx, node,
                                                          &control);

            item->current       = pre_result ? pre_result : node;
            item->skip_children = (control == NCC_XFORM_SKIP_CHILDREN);
            item->child_idx     = 0;
        }

        ncc_parse_tree_t *current = item->current;

        // Phase 2: Process children.
        size_t nch = ncc_pt_num_children(current);

        if (!item->skip_children && (size_t)item->child_idx < nch) {
            ncc_parse_tree_t *child = ncc_tree_child(
                current, item->child_idx);

            if (child) {
                // Skip subtrees rooted in system headers.
                if (node_in_system_header(child)) {
                    item->child_idx++;
                    continue;
                }

                // Grow stack if needed.
                if (stack_top >= stack_cap) {
                    int new_cap = stack_cap * 2;
                    walk_item_t *new_stack = ncc_alloc_array(
                        walk_item_t, new_cap);

                    if (!new_stack) {
                        ncc_free(stack);
                        return root;
                    }

                    memcpy(new_stack, stack,
                           (size_t)stack_top * sizeof(walk_item_t));
                    ncc_free(stack);
                    stack     = new_stack;
                    stack_cap = new_cap;
                    item      = &stack[stack_top - 1];
                }

                set_node_parent(child, current);

                stack[stack_top++] = (walk_item_t){
                    .node          = child,
                    .current       = child,
                    .child_idx     = -1,
                    .skip_children = false,
                };
                item->child_idx++;
                continue;
            }
            else {
                item->child_idx++;
                continue;
            }
        }

        // Phase 3: Post-order.
        if (item->skip_children) {
            item->child_idx = (int)nch;
        }

        if ((size_t)item->child_idx >= nch) {
            ncc_parse_tree_t *post_result = apply_post(reg, ctx, current);

            if (post_result) {
                current       = post_result;
                item->current = post_result;
            }

            ctx->depth--;

            ncc_parse_tree_t *result = item->current;
            ncc_parse_tree_t *orig   = item->node;
            stack_top--;

            // Update parent's child pointer if replaced.
            if (stack_top > 0 && result != orig) {
                walk_item_t *parent_item = &stack[stack_top - 1];
                int          child_ix    = parent_item->child_idx - 1;

                if (child_ix >= 0
                    && (size_t)child_ix
                           < ncc_pt_num_children(parent_item->current)) {
                    ncc_tree_set_child(parent_item->current,
                                         (size_t)child_ix, result);
                }
            }

            if (stack_top == 0) {
                final_result = result;
            }
        }
    }

    ncc_free(stack);
    return final_result;
}

// ============================================================================
// Public API: apply
// ============================================================================

ncc_parse_tree_t *
ncc_xform_apply(ncc_xform_registry_t *reg, ncc_xform_ctx_t *ctx)
{
    assert(reg && ctx);

    ctx->nodes_visited  = 0;
    ctx->nodes_replaced = 0;
    ctx->depth          = 0;

    if (!ctx->root) {
        return nullptr;
    }

    ncc_parse_tree_t *result = walk_tree(reg, ctx, ctx->root);

    if (result != ctx->root) {
        ctx->root = result;
    }

    return result;
}

ncc_parse_tree_t *
ncc_xform_apply_multi(ncc_xform_registry_t *reg,
                         ncc_xform_ctx_t      *ctx,
                         int                    max_passes)
{
    assert(reg && ctx);

    if (max_passes <= 0) {
        max_passes = 100;
    }

    ctx->pass = 0;

    while (ctx->pass < max_passes) {
        ncc_parse_tree_t *result = ncc_xform_apply(reg, ctx);

        if (ctx->nodes_replaced == 0) {
            break;
        }

        ctx->pass++;
        ctx->root = result;
    }

    return ctx->root;
}

// ============================================================================
// Parent pointer navigation
// ============================================================================

ncc_parse_tree_t *
ncc_xform_find_ancestor(ncc_parse_tree_t *node, const char *nt_name)
{
    if (!node || !nt_name) {
        return nullptr;
    }

    ncc_parse_tree_t *cur = node;

    while (cur && !ncc_tree_is_leaf(cur)) {
        ncc_nt_node_t pn = ncc_tree_node_value(cur);

        ncc_parse_tree_t *p = (ncc_parse_tree_t *)pn.parent;
        if (!p) {
            break;
        }

        if (!ncc_tree_is_leaf(p)) {
            ncc_nt_node_t ppn = ncc_tree_node_value(p);
            if (ppn.name.data && strcmp(ppn.name.data, nt_name) == 0) {
                return p;
            }
        }

        cur = p;
    }

    return nullptr;
}

// ============================================================================
// Template-based subtree construction
// ============================================================================

ncc_result_t(ncc_parse_tree_ptr_t)
ncc_xform_parse_template(ncc_grammar_t *grammar,
                           const char     *nt_name,
                           const char     *source,
                           ncc_scan_cb_t  tokenize)
{
    assert(grammar && nt_name && source);

    // Resolve the tokenizer: explicit param > grammar's stored cb.
    ncc_scan_cb_t cb = tokenize
                            ? tokenize
                            : (ncc_scan_cb_t)grammar->tokenize_cb;

    if (cb) {
        grammar->tokenize_cb = (void *)cb;
    }

    if (!cb) {
        return ncc_result_err(ncc_parse_tree_ptr_t,
                               NCC_XFORM_ERR_NO_TOKENIZER);
    }

    // Resolve the NT name to an id.
    ncc_string_t nt_str = ncc_string_from_cstr(nt_name);
    ncc_nonterm_t *nt   = ncc_nonterm(grammar, nt_str);

    if (!nt) {
        return ncc_result_err(ncc_parse_tree_ptr_t,
                               NCC_XFORM_ERR_UNKNOWN_NT);
    }

    int64_t nt_id = ncc_nonterm_id(nt);

    // Save the grammar's default start and override for PWZ init.
    int32_t saved_start    = grammar->default_start;
    grammar->default_start = (int32_t)nt_id;

    // Tokenize.
    size_t src_len          = strlen(source);
    ncc_buffer_t *buf      = ncc_buffer_from_bytes(source, (int64_t)src_len);
    ncc_scanner_t *scanner = ncc_scanner_new(buf, cb, grammar,
                                                ncc_option_none(ncc_string_t),
                                                nullptr, nullptr);
    ncc_token_stream_t *ts = ncc_token_stream_new(scanner);

    // Parse with PWZ (reads default_start during init).
    ncc_pwz_parser_t *parser = ncc_pwz_new(grammar);

    // Restore the grammar's original start.
    grammar->default_start = saved_start;

    bool ok = ncc_pwz_parse(parser, ts);

    ncc_result_t(ncc_parse_tree_ptr_t) ret;

    if (ok) {
        ncc_parse_tree_t *tree = ncc_pwz_get_tree(parser);
        ncc_parse_tree_t *cloned = ncc_xform_clone(tree);
        set_parent_pointers_recursive(cloned);
        ret = ncc_result_ok(ncc_parse_tree_ptr_t, cloned);
    }
    else {
        ret = ncc_result_err(ncc_parse_tree_ptr_t,
                              NCC_XFORM_ERR_PARSE_FAILED);
    }

    ncc_pwz_free(parser);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);

    return ret;
}
