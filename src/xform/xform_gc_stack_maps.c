// xform_gc_stack_maps.c -- Discover n00b GC stack roots for later emission.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
copy_cstr(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    char  *r   = ncc_alloc_size(1, len + 1);
    if (len > 0) {
        memcpy(r, s, len);
    }
    r[len] = '\0';
    return r;
}

static char *
trim_copy(const char *s)
{
    if (!s) {
        return copy_cstr("");
    }

    while (isspace((unsigned char)*s)) {
        s++;
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }

    char *result = ncc_alloc_size(1, len + 1);
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

static char *
node_text(ncc_parse_tree_t *node)
{
    ncc_string_t s = ncc_xform_node_to_text(node);
    char        *r = trim_copy(s.data);
    ncc_free(s.data);
    return r;
}

static bool
contains_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, text);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (contains_leaf_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

static int
count_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, text) ? 1 : 0;
    }

    int    result = 0;
    size_t nc     = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        result += count_leaf_text(ncc_tree_child(node, i), text);
    }

    return result;
}

typedef struct {
    ncc_parse_tree_t **data;
    size_t             len;
    size_t             cap;
} parse_tree_list_t;

static void
parse_tree_list_push(parse_tree_list_t *list, ncc_parse_tree_t *node)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(ncc_parse_tree_t *));
        list->cap = new_cap;
    }

    list->data[list->len++] = node;
}

static void
collect_nt_children(ncc_parse_tree_t *node, const char *name,
                    parse_tree_list_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        parse_tree_list_push(out, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_nt_children(ncc_tree_child(node, i), name, out);
    }
}

static bool
child_index(ncc_parse_tree_t *parent, ncc_parse_tree_t *child,
            size_t *idx_out)
{
    if (!parent || !child || ncc_tree_is_leaf(parent)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(parent);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_tree_child(parent, i) == child) {
            if (idx_out) {
                *idx_out = i;
            }
            return true;
        }
    }

    return false;
}

static ncc_token_info_t *
first_leaf_token(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_tree_leaf_value(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *tok = first_leaf_token(ncc_tree_child(node, i));
        if (tok) {
            return tok;
        }
    }

    return nullptr;
}

static void
add_token_trivia(ncc_token_info_t *tok, const char *text, bool leading)
{
    if (!tok || !text) {
        return;
    }

    ncc_trivia_t *trivia = ncc_alloc(ncc_trivia_t);
    trivia->text = ncc_string_from_cstr(text);

    if (leading) {
        trivia->next = tok->leading_trivia;
        tok->leading_trivia = trivia;
    }
    else {
        trivia->next = tok->trailing_trivia;
        tok->trailing_trivia = trivia;
    }
}

static void
add_generated_spacing(ncc_parse_tree_t *node, bool leading)
{
    if (leading) {
        add_token_trivia(first_leaf_token(node), "\n", true);
    }

    add_token_trivia(ncc_xform_find_last_leaf_token(node), "\n", false);
}

static bool
is_group_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return pn.group_top || pn.group_item
        || (pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$');
}

static ncc_parse_tree_t *
node_parent(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return (ncc_parse_tree_t *)pn.parent;
}

static ncc_parse_tree_t *
declaration_block_item(ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *decl = ncc_xform_find_ancestor(node, "declaration");
    if (!decl) {
        return nullptr;
    }

    ncc_parse_tree_t *cur = decl;
    for (;;) {
        ncc_parse_tree_t *parent = node_parent(cur);
        if (!parent) {
            return nullptr;
        }

        if (ncc_xform_nt_name_is(parent, "block_item")) {
            return parent;
        }

        if (!is_group_node(parent)) {
            return nullptr;
        }

        cur = parent;
    }
}

static ncc_parse_tree_t *
first_descendant_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found = first_descendant_nt(ncc_tree_child(node, i),
                                                      name);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static char *
declarator_name(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (text && (isalpha((unsigned char)text[0]) || text[0] == '_')) {
            return copy_cstr(text);
        }
        return nullptr;
    }

    char  *last = nullptr;
    size_t nc   = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = declarator_name(ncc_tree_child(node, i));
        if (candidate) {
            ncc_free(last);
            last = candidate;
        }
    }
    return last;
}

static bool
is_ident_start_char(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool
is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static char *
copy_last_identifier_before(const char *text, size_t end)
{
    size_t i = end;

    while (i > 0) {
        while (i > 0 && !is_ident_char(text[i - 1])) {
            i--;
        }
        size_t ident_end = i;
        while (i > 0 && is_ident_char(text[i - 1])) {
            i--;
        }
        if (ident_end > i && is_ident_start_char(text[i])) {
            size_t len  = ident_end - i;
            char  *name = ncc_alloc_size(1, len + 1);
            memcpy(name, text + i, len);
            name[len] = '\0';
            return name;
        }
    }

    return nullptr;
}

static size_t
find_last_top_level_param_open(const char *text)
{
    int    depth = 0;
    size_t last  = (size_t)-1;

    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '(') {
            if (depth == 0) {
                last = i;
            }
            depth++;
        }
        else if (text[i] == ')' && depth > 0) {
            depth--;
        }
    }

    return last;
}

static char *
function_name(ncc_parse_tree_t *declarator)
{
    char *text = node_text(declarator);
    if (!text) {
        return copy_cstr("<unknown>");
    }

    size_t param_open = find_last_top_level_param_open(text);
    size_t name_end   = param_open == (size_t)-1 ? strlen(text) : param_open;
    char  *name       = copy_last_identifier_before(text, name_end);

    ncc_free(text);
    return name ? name : copy_cstr("<unknown>");
}

static void
gc_stack_errorf(ncc_parse_tree_t *node, const char *fmt, ...)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(node, &line, &col);

    fprintf(stderr, "ncc: error: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (line %u, col %u)\n", line, col);
    exit(1);
}

static bool
decl_is_stack_storage(ncc_parse_tree_t *decl_specs)
{
    return !contains_leaf_text(decl_specs, "static")
        && !contains_leaf_text(decl_specs, "extern")
        && !contains_leaf_text(decl_specs, "typedef");
}

static bool
decl_specs_are_aggregate(ncc_parse_tree_t *decl_specs)
{
    return first_descendant_nt(decl_specs, "struct_or_union_specifier")
        != nullptr;
}

static bool
parse_positive_integer_literal(const char *text, uint64_t *out)
{
    char *trimmed = trim_copy(text);
    if (!trimmed[0]) {
        ncc_free(trimmed);
        return false;
    }

    for (char *p = trimmed; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            ncc_free(trimmed);
            return false;
        }
    }

    char    *end = nullptr;
    uint64_t val = strtoull(trimmed, &end, 10);
    bool     ok  = end && *end == '\0' && val > 0;
    ncc_free(trimmed);

    if (ok) {
        *out = val;
    }
    return ok;
}

static bool
array_bound_product(ncc_parse_tree_t *node, uint64_t *product,
                    ncc_parse_tree_t **bad_array)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return true;
    }

    if (ncc_xform_nt_name_is(node, "array_declarator")) {
        ncc_parse_tree_t *bound = ncc_xform_find_child_nt(
            node, "assignment_expression");
        uint64_t count = 0;

        if (!bound) {
            *bad_array = node;
            return false;
        }

        char *text = node_text(bound);
        bool  ok   = parse_positive_integer_literal(text, &count);
        ncc_free(text);

        if (!ok) {
            *bad_array = node;
            return false;
        }

        *product *= count;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (!array_bound_product(ncc_tree_child(node, i), product,
                                 bad_array)) {
            return false;
        }
    }

    return true;
}

static bool
has_parenthesized_pointer_array(ncc_parse_tree_t *declarator)
{
    char *text = node_text(declarator);
    bool  result = strstr(text, "(*") != nullptr || strstr(text, "( *")
                                                     != nullptr;
    ncc_free(text);
    return result;
}

static ncc_gc_stack_root_t *
record_root(ncc_xform_ctx_t *ctx, const char *function, const char *name,
            ncc_parse_tree_t *type_node, ncc_gc_stack_root_kind_t kind,
            ncc_gc_stack_root_shape_t shape, uint64_t num_words,
            ncc_parse_tree_t *scope, ncc_parse_tree_t *decl_node)
{
    ncc_xform_data_t    *data = ncc_xform_get_data(ctx);
    ncc_gc_stack_root_t *root = ncc_alloc(ncc_gc_stack_root_t);

    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(decl_node, &line, &col);

    *root = (ncc_gc_stack_root_t){
        .function_name = copy_cstr(function),
        .name          = copy_cstr(name),
        .type_text     = node_text(type_node),
        .kind          = kind,
        .shape         = shape,
        .num_words     = num_words,
        .line          = line,
        .col           = col,
        .scope         = scope,
        .declaration   = decl_node,
        .next          = data->gc_stack_roots,
    };

    data->gc_stack_roots = root;
    data->gc_stack_root_count++;
    return root;
}

static void
classify_declarator(ncc_xform_ctx_t *ctx, const char *function,
                    ncc_parse_tree_t *decl_specs,
                    ncc_parse_tree_t *declarator,
                    ncc_gc_stack_root_kind_t kind, ncc_parse_tree_t *scope,
                    ncc_parse_tree_t *decl_node)
{
    if (!decl_specs || !declarator) {
        return;
    }

    int  ptr_depth = count_leaf_text(declarator, "*")
                   + count_leaf_text(declarator, "^");
    bool is_array  = first_descendant_nt(declarator, "array_declarator")
                  != nullptr;
    bool aggregate = decl_specs_are_aggregate(decl_specs);

    if (kind == NCC_GC_STACK_ROOT_PARAM && is_array) {
        ptr_depth = ptr_depth > 0 ? ptr_depth : 1;
        is_array  = false;
    }

    if (ptr_depth == 0 && !is_array && !aggregate) {
        return;
    }

    if (is_array && ptr_depth == 0 && !aggregate) {
        return;
    }

    char *name = declarator_name(declarator);
    if (!name) {
        gc_stack_errorf(declarator,
                        "GC stack-map root in function '%s' is unsupported: "
                        "pointer or aggregate parameter has no name",
                        function);
    }

    ncc_gc_stack_root_shape_t shape     = NCC_GC_STACK_ROOT_POINTER;
    uint64_t                  num_words = 1;

    if (is_array) {
        if (ptr_depth > 0 && has_parenthesized_pointer_array(declarator)) {
            gc_stack_errorf(declarator,
                            "GC stack-map root '%s' in function '%s' uses a "
                            "parenthesized pointer/array declarator; spell the "
                            "root as a direct fixed-size pointer array or a "
                            "single pointer",
                            name, function);
        }

        ncc_parse_tree_t *bad_array = nullptr;
        uint64_t          product   = 1;
        if (!array_bound_product(declarator, &product, &bad_array)) {
            gc_stack_errorf(bad_array ? bad_array : declarator,
                            "GC stack-map root '%s' in function '%s' uses a "
                            "variable-length or incomplete array; only "
                            "fixed-size stack roots are supported",
                            name, function);
        }

        num_words = product;
        shape     = ptr_depth > 0 ? NCC_GC_STACK_ROOT_POINTER_ARRAY
                                  : NCC_GC_STACK_ROOT_AGGREGATE;
    }
    else if (aggregate) {
        shape     = NCC_GC_STACK_ROOT_AGGREGATE;
        num_words = 0;
    }

    if (kind == NCC_GC_STACK_ROOT_LOCAL
        && !declaration_block_item(declarator)) {
        gc_stack_errorf(declarator,
                        "GC stack-map root '%s' in function '%s' is declared "
                        "in an unsupported statement context; declare the root "
                        "as a block item before the statement",
                        name, function);
    }

    record_root(ctx, function, name, decl_specs, kind, shape, num_words,
                scope, decl_node);
    ncc_free(name);
}

static void
scan_parameter_node(ncc_xform_ctx_t *ctx, const char *function,
                    ncc_parse_tree_t *scope, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "parameter_declaration")) {
        ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
            node, "declaration_specifiers");
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            node, "declarator");
        if (!declarator) {
            declarator = ncc_xform_find_child_nt(node,
                                                 "abstract_declarator");
        }

        classify_declarator(ctx, function, decl_specs, declarator,
                            NCC_GC_STACK_ROOT_PARAM, scope, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_parameter_node(ctx, function, scope, ncc_tree_child(node, i));
    }
}

static void
scan_init_declarators(ncc_xform_ctx_t *ctx, const char *function,
                      ncc_parse_tree_t *decl_specs, ncc_parse_tree_t *scope,
                      ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "init_declarator")) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            node, "declarator");
        classify_declarator(ctx, function, decl_specs, declarator,
                            NCC_GC_STACK_ROOT_LOCAL, scope, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_init_declarators(ctx, function, decl_specs, scope,
                              ncc_tree_child(node, i));
    }
}

static void
scan_declaration(ncc_xform_ctx_t *ctx, const char *function,
                 ncc_parse_tree_t *decl)
{
    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        decl, "declaration_specifiers");
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!decl_specs || !list || !decl_is_stack_storage(decl_specs)) {
        return;
    }

    ncc_parse_tree_t *scope = ncc_xform_find_ancestor(decl,
                                                      "compound_statement");
    scan_init_declarators(ctx, function, decl_specs, scope, list);
}

static void
scan_compound(ncc_xform_ctx_t *ctx, const char *function,
              ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "declaration")) {
        scan_declaration(ctx, function, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_compound(ctx, function, ncc_tree_child(node, i));
    }
}

static ncc_parse_tree_t *
xform_gc_stack_function(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *fn)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->gc_stack_maps) {
        return nullptr;
    }

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(fn, "declarator");
    ncc_parse_tree_t *body       = ncc_xform_find_child_nt(fn,
                                                           "function_body");
    ncc_parse_tree_t *compound   = ncc_xform_find_child_nt(body,
                                                           "compound_statement");
    if (!declarator || !compound) {
        return nullptr;
    }

    char *fname = function_name(declarator);
    if (strncmp(fname, "n00b_gc_stack_", 14) == 0
        || strncmp(fname, "__ncc_gc_stack_", 16) == 0) {
        ncc_free(fname);
        return nullptr;
    }

    ncc_parse_tree_t *params = first_descendant_nt(declarator,
                                                   "parameter_type_list");
    if (params) {
        scan_parameter_node(ctx, fname, compound, params);
    }

    scan_compound(ctx, fname, compound);
    ncc_free(fname);
    return nullptr;
}

typedef enum {
    GC_FRAME_GROUP_PARAMS,
    GC_FRAME_GROUP_LOCAL,
} gc_frame_group_kind_t;

typedef struct {
    gc_frame_group_kind_t kind;
    ncc_parse_tree_t     *anchor;
    ncc_parse_tree_t     *scope;
    int                   id;
} gc_frame_group_t;

typedef struct {
    gc_frame_group_t *data;
    size_t            len;
    size_t            cap;
} gc_frame_group_list_t;

static void
gc_frame_group_list_push(gc_frame_group_list_t *list, gc_frame_group_t group)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(gc_frame_group_t));
        list->cap = new_cap;
    }

    list->data[list->len++] = group;
}

static ncc_parse_tree_t *
root_group_anchor(ncc_gc_stack_root_t *root, gc_frame_group_kind_t *kind)
{
    if (root->kind == NCC_GC_STACK_ROOT_PARAM) {
        *kind = GC_FRAME_GROUP_PARAMS;
        return root->scope;
    }

    *kind = GC_FRAME_GROUP_LOCAL;
    return declaration_block_item(root->declaration);
}

static bool
group_matches_root(gc_frame_group_t *group, ncc_gc_stack_root_t *root)
{
    gc_frame_group_kind_t kind   = GC_FRAME_GROUP_LOCAL;
    ncc_parse_tree_t     *anchor = root_group_anchor(root, &kind);

    return group->kind == kind && group->anchor == anchor;
}

static void
ensure_group_for_root(ncc_xform_ctx_t *ctx, gc_frame_group_list_t *groups,
                      ncc_gc_stack_root_t *root)
{
    gc_frame_group_kind_t kind   = GC_FRAME_GROUP_LOCAL;
    ncc_parse_tree_t     *anchor = root_group_anchor(root, &kind);

    if (!anchor) {
        gc_stack_errorf(root->declaration,
                        "GC stack-map root '%s' in function '%s' has no "
                        "supported insertion point",
                        root->name, root->function_name);
    }

    for (size_t i = 0; i < groups->len; i++) {
        if (groups->data[i].kind == kind && groups->data[i].anchor == anchor) {
            return;
        }
    }

    gc_frame_group_list_push(groups, (gc_frame_group_t){
        .kind   = kind,
        .anchor = anchor,
        .scope  = root->scope,
        .id     = ctx->unique_id++,
    });
}

static size_t
count_group_roots(gc_frame_group_t *group, ncc_gc_stack_root_t *roots)
{
    size_t count = 0;

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (group_matches_root(group, root)) {
            count++;
        }
    }

    return count;
}

static char *
c_string_literal(const char *text)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_putc(buf, '"');

    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        switch (*p) {
        case '\\':
            ncc_buffer_puts(buf, "\\\\");
            break;
        case '"':
            ncc_buffer_puts(buf, "\\\"");
            break;
        case '\n':
            ncc_buffer_puts(buf, "\\n");
            break;
        case '\r':
            ncc_buffer_puts(buf, "\\r");
            break;
        case '\t':
            ncc_buffer_puts(buf, "\\t");
            break;
        default:
            if (*p < 0x20 || *p == 0x7f) {
                ncc_buffer_printf(buf, "\\x%02x", (unsigned)*p);
            }
            else {
                ncc_buffer_putc(buf, (char)*p);
            }
            break;
        }
    }

    ncc_buffer_putc(buf, '"');
    return ncc_buffer_take(buf);
}

static void
append_slot_num_words(ncc_buffer_t *buf, ncc_gc_stack_root_t *root)
{
    if (root->shape == NCC_GC_STACK_ROOT_AGGREGATE) {
        ncc_buffer_printf(buf,
                          "((sizeof(%s)+sizeof(void *)-1)/sizeof(void *))",
                          root->name);
        return;
    }

    ncc_buffer_printf(buf, "%llu", (unsigned long long)root->num_words);
}

static char *
build_frame_source(gc_frame_group_t *group, ncc_gc_stack_root_t *roots)
{
    size_t        count    = count_group_roots(group, roots);
    ncc_buffer_t *buf      = ncc_buffer_empty();
    const char   *function = "<unknown>";
    uint32_t      line     = 0;

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (group_matches_root(group, root)) {
            function = root->function_name;
            line     = root->line;
            break;
        }
    }

    char *function_lit = c_string_literal(function);

    ncc_buffer_printf(buf,
                      "static const n00b_gc_stack_slot_t "
                      "__ncc_gc_slots_%d[] = {",
                      group->id);

    size_t root_index = 0;
    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (!group_matches_root(group, root)) {
            continue;
        }

        ncc_buffer_printf(buf, "{.root_index=%zu,.num_words=", root_index);
        append_slot_num_words(buf, root);
        ncc_buffer_puts(buf, "},");
        root_index++;
    }

    ncc_buffer_puts(buf, "};");
    ncc_buffer_printf(buf,
                      "static const n00b_gc_stack_map_t __ncc_gc_map_%d="
                      "{.num_roots=%zu,.num_slots=%zu,"
                      ".slots=__ncc_gc_slots_%d,"
                      ".function_name=%s,.file_name=__FILE__,.line=%u};",
                      group->id, count, count, group->id, function_lit, line);
    ncc_buffer_printf(buf, "void *__ncc_gc_roots_%d[]={", group->id);

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (!group_matches_root(group, root)) {
            continue;
        }

        ncc_buffer_printf(buf, "(void *)&%s,", root->name);
    }

    ncc_buffer_puts(buf, "};");
    ncc_buffer_printf(buf,
                      "n00b_gc_stack_frame_t __ncc_gc_frame_%d "
                      "__attribute__((cleanup(__ncc_gc_stack_pop_cleanup)));",
                      group->id);
    ncc_buffer_printf(buf,
                      "n00b_gc_stack_push(&__ncc_gc_frame_%d,"
                      "&__ncc_gc_map_%d,__ncc_gc_roots_%d);",
                      group->id, group->id, group->id);

    ncc_free(function_lit);
    return ncc_buffer_take(buf);
}

static parse_tree_list_t
parse_generated_block_items(ncc_xform_ctx_t *ctx, const char *src)
{
    parse_tree_list_t result = {0};
    ncc_parse_tree_t *tree = ncc_xform_parse_source(ctx->grammar,
                                                    "block_item_list", src,
                                                    "xform_gc_stack_maps");
    if (!tree) {
        fprintf(stderr,
                "ncc: error: failed to parse generated GC stack-map block:\n"
                "%s\n",
                src);
        exit(1);
    }

    collect_nt_children(tree, "block_item", &result);
    if (result.len == 0 && ncc_xform_nt_name_is(tree, "block_item")) {
        parse_tree_list_push(&result, tree);
    }

    for (size_t i = 0; i < result.len; i++) {
        add_generated_spacing(result.data[i], i == 0);
    }

    return result;
}

static void
insert_block_items_at(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *compound,
                      size_t index,
                      parse_tree_list_t *items)
{
    ncc_parse_tree_t *bil = ncc_xform_find_child_nt(compound,
                                                    "block_item_list");

    if (!bil) {
        ncc_parse_tree_t **children = ncc_alloc_size(
            items->len, sizeof(ncc_parse_tree_t *));
        for (size_t i = 0; i < items->len; i++) {
            children[i] = items->data[i];
        }

        bil = ncc_xform_make_node_with_children(ctx->grammar,
                                                "block_item_list", 0,
                                                children, items->len);
        ncc_free(children);

        size_t nc = ncc_tree_num_children(compound);
        ncc_xform_insert_child(compound, nc > 0 ? nc - 1 : 0, bil);
        return;
    }

    for (size_t i = 0; i < items->len; i++) {
        ncc_xform_insert_child(bil, index + i, items->data[i]);
    }
}

static void
insert_frame_group(ncc_xform_ctx_t *ctx, gc_frame_group_t *group,
                   ncc_gc_stack_root_t *roots)
{
    char *src = build_frame_source(group, roots);
    parse_tree_list_t items = parse_generated_block_items(ctx, src);
    ncc_free(src);

    if (group->kind == GC_FRAME_GROUP_PARAMS) {
        insert_block_items_at(ctx, group->anchor, 0, &items);
        ncc_free(items.data);
        return;
    }

    ncc_nt_node_t    pn        = ncc_tree_node_value(group->anchor);
    ncc_parse_tree_t *container = (ncc_parse_tree_t *)pn.parent;
    size_t            anchor_ix = 0;

    if (!container || !child_index(container, group->anchor, &anchor_ix)) {
        gc_stack_errorf(group->anchor,
                        "GC stack-map frame insertion failed: local root "
                        "declaration is not in a block item list");
    }

    for (size_t i = 0; i < items.len; i++) {
        ncc_xform_insert_child(container, anchor_ix + 1 + i, items.data[i]);
    }

    ncc_free(items.data);
}

static void
insert_helper_before_first_rooted_function(ncc_xform_ctx_t *ctx,
                                           ncc_gc_stack_root_t *roots)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (data->gc_stack_maps_helper_inserted) {
        return;
    }

    ncc_parse_tree_t *best_container = nullptr;
    ncc_parse_tree_t *best_ext       = nullptr;
    size_t            best_ix        = 0;
    int32_t           best_start     = 0;

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        ncc_parse_tree_t *ext = ncc_xform_find_ancestor(root->scope,
                                                        "external_declaration");
        if (!ext) {
            continue;
        }

        ncc_nt_node_t    epn       = ncc_tree_node_value(ext);
        ncc_parse_tree_t *container = epn.parent
                                          ? (ncc_parse_tree_t *)epn.parent
                                          : ctx->root;
        size_t ix = 0;
        if (!child_index(container, ext, &ix)) {
            continue;
        }

        if (!best_ext || epn.start < best_start) {
            best_container = container;
            best_ext       = ext;
            best_ix        = ix;
            best_start     = epn.start;
        }
    }

    if (!best_ext || !best_container) {
        gc_stack_errorf(ctx->root,
                        "GC stack-map helper insertion failed: no rooted "
                        "function was found in the translation unit");
    }

    const char *src =
        "static inline void "
        "__ncc_gc_stack_pop_cleanup(n00b_gc_stack_frame_t *frame)"
        "{n00b_gc_stack_pop(frame);}";
    ncc_parse_tree_t *helper = ncc_xform_parse_source(ctx->grammar,
                                                      "external_declaration",
                                                      src,
                                                      "xform_gc_stack_maps");
    if (!helper) {
        fprintf(stderr,
                "ncc: error: failed to parse generated GC stack-map helper:\n"
                "%s\n",
                src);
        exit(1);
    }

    add_generated_spacing(helper, true);
    ncc_xform_insert_child(best_container, best_ix, helper);
    data->gc_stack_maps_helper_inserted = true;
}

static ncc_parse_tree_t *
xform_gc_stack_translation_unit(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->gc_stack_maps || !data->gc_stack_roots) {
        return nullptr;
    }

    insert_helper_before_first_rooted_function(ctx, data->gc_stack_roots);

    gc_frame_group_list_t groups = {0};
    for (ncc_gc_stack_root_t *root = data->gc_stack_roots; root;
         root = root->next) {
        ensure_group_for_root(ctx, &groups, root);
    }

    for (size_t i = 0; i < groups.len; i++) {
        insert_frame_group(ctx, &groups.data[i], data->gc_stack_roots);
    }

    ncc_free(groups.data);
    (void)tu;
    return nullptr;
}

void
ncc_register_gc_stack_maps_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "function_definition", xform_gc_stack_function,
                       "gc_stack_maps");
    ncc_xform_register(reg, "translation_unit",
                       xform_gc_stack_translation_unit,
                       "gc_stack_maps_emit");
}
