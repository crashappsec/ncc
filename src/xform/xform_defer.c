// xform_defer.c — `_defer <secondary-block>` lowering (N3199 semantics).
//
// `_defer S` registers S when control reaches it and runs S at exit of the
// enclosing block, in LIFO order, on every exit path. The block runs with
// current variable values (no capture). `_defer` is spelled with a leading
// underscore to avoid colliding with libn00b's `defer` macro.
//
// Lowering (clang has no nested functions, so __attribute__((cleanup)) cannot
// host an inline body that references locals): the deferred bodies are injected
// as source at each scope exit. In structured code the set of *registered*
// defers at any exit is statically the textually-preceding defers of the
// enclosing scopes, so no runtime flags are needed — this pass walks the body
// once, tracking a scope stack of active defer bodies, and emits them reversed:
//   - return E;   ->  { typeof(E) __ncc_defer_ret = (E); <all scopes>; return __ncc_defer_ret; }
//   - break;      ->  { <scopes up to & incl. nearest loop/switch>; break; }
//   - continue;   ->  { <scopes up to & incl. nearest loop>; continue; }
//   - fall-through->  the enclosing block's defers, reversed, before its `}`.
//
// v1 scope (see commit message): return / break / continue / fall-through and
// nested blocks are fully handled. `goto` (and labeled break/continue, computed
// goto) in a region with active defers, and control transfer OUT of a defer
// body, are rejected with a diagnostic rather than mis-compiled.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/parse_tree.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------------------
// Scope stack
// ----------------------------------------------------------------------------

typedef enum {
    DS_FUNC,    // the function body
    DS_BLOCK,   // an ordinary compound statement (incl. if/else bodies)
    DS_LOOP,    // for / while / do body
    DS_SWITCH,  // switch body
} dscope_kind_t;

typedef struct dscope {
    dscope_kind_t   kind;
    char          **defers;  // active defer body texts, registration order
    size_t          len;
    size_t          cap;
    struct dscope  *parent;
} dscope_t;

static void
dscope_register(dscope_t *s, char *body_text)
{
    if (s->len >= s->cap) {
        s->cap    = s->cap ? s->cap * 2 : 4;
        s->defers = ncc_realloc(s->defers, s->cap * sizeof(char *));
    }
    s->defers[s->len++] = body_text;
}

static void
dscope_free(dscope_t *s)
{
    for (size_t i = 0; i < s->len; i++) {
        ncc_free(s->defers[i]);
    }
    ncc_free(s->defers);
}

// Emit one scope's active defers, LIFO (reverse registration order). Each body
// is wrapped in its own braces so a single-statement defer and a compound defer
// lower identically and the body cannot leak declarations into the exit site.
static void
emit_scope_defers(ncc_buffer_t *buf, dscope_t *s)
{
    for (size_t i = s->len; i-- > 0;) {
        ncc_buffer_puts(buf, "{ ");
        ncc_buffer_puts(buf, s->defers[i]);
        ncc_buffer_puts(buf, " } ");
    }
}

// ----------------------------------------------------------------------------
// Subtree predicates
// ----------------------------------------------------------------------------

static bool
subtree_has_nt(ncc_parse_tree_t *node, const char *nt)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    if (ncc_xform_nt_name_is(node, nt)) {
        return true;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_nt(ncc_tree_child(node, i), nt)) {
            return true;
        }
    }
    return false;
}

// A jump this pass cannot yet combine with _defer: `goto` (plain or computed),
// or a labeled `break`/`continue`. Correctly running the right defers across
// such a jump needs label-scope analysis (a planned follow-up), so a function
// mixing these with _defer is rejected rather than mis-lowered.
static bool
is_unsupported_jump(ncc_parse_tree_t *node)
{
    if (!ncc_xform_nt_name_is(node, "jump_statement")) {
        return false;
    }
    const char *kw = ncc_xform_get_first_leaf_text(node);
    if (kw && strcmp(kw, "goto") == 0) {
        return true;
    }
    // Labeled break/continue carry an identifier operand.
    if (kw && (strcmp(kw, "break") == 0 || strcmp(kw, "continue") == 0)) {
        return ncc_xform_find_child_nt(node, "identifier") != nullptr;
    }
    return false;
}

static bool
subtree_has_unsupported_jump(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    if (is_unsupported_jump(node)) {
        return true;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_unsupported_jump(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

static void
defer_error(ncc_parse_tree_t *at, const char *msg)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(at, &line, &col);
    fprintf(stderr, "ncc: error: %s (line %u, col %u)\n", msg, line, col);
    exit(1);
}

// ----------------------------------------------------------------------------
// Emitter
// ----------------------------------------------------------------------------

static void emit_item(ncc_buffer_t *buf, ncc_parse_tree_t *node, dscope_t *scope,
                      bool *exited);
static void emit_compound(ncc_buffer_t *buf, ncc_parse_tree_t *compound,
                          dscope_t *parent, dscope_kind_t kind);
static void emit_body(ncc_buffer_t *buf, ncc_parse_tree_t *secondary_block,
                      dscope_t *parent, dscope_kind_t kind);

// Run defers from `scope` outward to the function root (used by `return`).
static void
emit_defers_to_func(ncc_buffer_t *buf, dscope_t *scope)
{
    for (dscope_t *s = scope; s; s = s->parent) {
        emit_scope_defers(buf, s);
    }
}

// Run defers from `scope` outward, stopping after the nearest loop (continue)
// or loop/switch (break).
static void
emit_defers_to_loop(ncc_buffer_t *buf, dscope_t *scope, bool stop_at_switch)
{
    for (dscope_t *s = scope; s; s = s->parent) {
        emit_scope_defers(buf, s);
        if (s->kind == DS_LOOP || (stop_at_switch && s->kind == DS_SWITCH)) {
            return;
        }
    }
}

// Lower a jump_statement. Sets *exited when it is an unconditional exit.
static void
emit_jump(ncc_buffer_t *buf, ncc_parse_tree_t *node, dscope_t *scope,
          bool *exited)
{
    const char *kw = ncc_xform_get_first_leaf_text(node);

    if (kw && strcmp(kw, "return") == 0) {
        ncc_parse_tree_t *expr = ncc_xform_find_child_nt(node, "expression");
        ncc_buffer_puts(buf, "{ ");
        if (expr) {
            ncc_string_t etext = ncc_xform_node_to_text(expr);
            ncc_buffer_puts(buf, "typeof (");
            ncc_buffer_puts(buf, etext.data ? etext.data : "");
            ncc_buffer_puts(buf, ") __ncc_defer_ret = (");
            ncc_buffer_puts(buf, etext.data ? etext.data : "");
            ncc_buffer_puts(buf, "); ");
            if (etext.data) {
                ncc_free(etext.data);
            }
            emit_defers_to_func(buf, scope);
            ncc_buffer_puts(buf, "return __ncc_defer_ret; }");
        }
        else {
            emit_defers_to_func(buf, scope);
            ncc_buffer_puts(buf, "return; }");
        }
        *exited = true;
        return;
    }

    // Plain `break;` / `continue;` (no label).
    bool labeled = ncc_xform_find_child_nt(node, "identifier") != nullptr;
    if (!labeled && kw && strcmp(kw, "break") == 0) {
        ncc_buffer_puts(buf, "{ ");
        emit_defers_to_loop(buf, scope, /*stop_at_switch=*/true);
        ncc_buffer_puts(buf, "break; }");
        *exited = true;
        return;
    }
    if (!labeled && kw && strcmp(kw, "continue") == 0) {
        ncc_buffer_puts(buf, "{ ");
        emit_defers_to_loop(buf, scope, /*stop_at_switch=*/false);
        ncc_buffer_puts(buf, "continue; }");
        *exited = true;
        return;
    }

    // goto / computed goto / labeled break/continue: rejected up front for any
    // _defer-using function (see xform_defer_funcdef), so this is unreachable.
    defer_error(node,
                "goto / labeled break/continue with _defer is not yet "
                "supported");
    *exited = true;
}

// Emit any statement/block-item, descending transparently through wrapper
// nonterminals. Sets *exited if this item is an unconditional exit (so the
// caller can suppress unreachable fall-through defers).
static void
emit_item(ncc_buffer_t *buf, ncc_parse_tree_t *node, dscope_t *scope,
          bool *exited)
{
    if (!node) {
        return;
    }
    if (ncc_tree_is_leaf(node)) {
        const char *t = ncc_xform_leaf_text(node);
        if (t) {
            ncc_buffer_puts(buf, t);
            ncc_buffer_putc(buf, ' ');
        }
        return;
    }

    if (ncc_xform_nt_name_is(node, "defer_statement")) {
        ncc_parse_tree_t *sb = ncc_xform_find_child_nt(node, "secondary_block");
        if (!sb) {
            return;
        }
        if (subtree_has_nt(sb, "jump_statement")) {
            // A return/break/continue/goto that transfers control OUT of the
            // defer body is forbidden; an in-body loop's own break is rare and
            // also rejected conservatively for v1.
            defer_error(node,
                        "control transfer (return/break/continue/goto) inside a "
                        "_defer body is not allowed");
        }
        ncc_string_t body = ncc_xform_node_to_text(sb);
        char        *bt   = body.data;
        if (!bt) {
            bt    = ncc_alloc_size(1, 1);
            bt[0] = '\0';
        }
        dscope_register(scope, bt);
        return;
    }

    if (ncc_xform_nt_name_is(node, "compound_statement")) {
        emit_compound(buf, node, scope, DS_BLOCK);
        return;
    }

    if (ncc_xform_nt_name_is(node, "jump_statement")) {
        emit_jump(buf, node, scope, exited);
        return;
    }

    if (ncc_xform_nt_name_is(node, "iteration_statement")) {
        size_t nc = ncc_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            ncc_parse_tree_t *c = ncc_tree_child(node, i);
            if (c && !ncc_tree_is_leaf(c)
                && ncc_xform_nt_name_is(c, "secondary_block")) {
                emit_body(buf, c, scope, DS_LOOP);
            }
            else {
                bool sub_exit = false;
                emit_item(buf, c, scope, &sub_exit);
            }
        }
        return;
    }

    if (ncc_xform_nt_name_is(node, "selection_statement")) {
        const char  *kw   = ncc_xform_get_first_leaf_text(node);
        dscope_kind_t bk  = (kw && strcmp(kw, "switch") == 0) ? DS_SWITCH
                                                              : DS_BLOCK;
        size_t        nc  = ncc_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            ncc_parse_tree_t *c = ncc_tree_child(node, i);
            if (c && !ncc_tree_is_leaf(c)
                && ncc_xform_nt_name_is(c, "secondary_block")) {
                emit_body(buf, c, scope, bk);
            }
            else {
                bool sub_exit = false;
                emit_item(buf, c, scope, &sub_exit);
            }
        }
        return;
    }

    // A plain statement / declaration / wrapper. If it contains nothing this
    // pass must rewrite, emit it verbatim; otherwise descend into its children
    // (this transparently handles block_item / unlabeled_statement /
    // primary_block / statement / labeled_statement wrappers).
    if (!subtree_has_nt(node, "defer_statement")
        && !subtree_has_nt(node, "jump_statement")) {
        ncc_string_t t = ncc_xform_node_to_text(node);
        ncc_buffer_puts(buf, t.data ? t.data : "");
        ncc_buffer_putc(buf, ' ');
        if (t.data) {
            ncc_free(t.data);
        }
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        emit_item(buf, ncc_tree_child(node, i), scope, exited);
    }
}

// Emit a compound statement, opening a fresh scope of `kind`.
static void
emit_compound(ncc_buffer_t *buf, ncc_parse_tree_t *compound, dscope_t *parent,
              dscope_kind_t kind)
{
    dscope_t scope = {.kind = kind, .parent = parent};

    ncc_buffer_puts(buf, "{ ");

    ncc_parse_tree_t *list = ncc_xform_find_child_nt(compound,
                                                     "block_item_list");
    bool exited = false;
    if (list) {
        size_t nc = ncc_tree_num_children(list);
        for (size_t i = 0; i < nc; i++) {
            exited = false;
            emit_item(buf, ncc_tree_child(list, i), &scope, &exited);
        }
    }

    // Fall-through: run this scope's defers (reversed). Skip if the block ended
    // in an unconditional exit (the defers were already emitted there, and
    // emitting again would be unreachable).
    if (!exited) {
        emit_scope_defers(buf, &scope);
    }

    ncc_buffer_puts(buf, "}");
    dscope_free(&scope);
}

// Emit a loop/if/switch body (a secondary_block), giving its block the scope
// kind so break/continue resolve to it. A single-statement body is wrapped in a
// synthetic block so a defer in it still runs at body end.
static void
emit_body(ncc_buffer_t *buf, ncc_parse_tree_t *secondary_block,
          dscope_t *parent, dscope_kind_t kind)
{
    ncc_parse_tree_t *compound = ncc_xform_find_child_nt(secondary_block,
                                                         "compound_statement");
    if (compound) {
        emit_compound(buf, compound, parent, kind);
        return;
    }

    // Single-statement body.
    dscope_t scope  = {.kind = kind, .parent = parent};
    bool     exited = false;
    ncc_buffer_puts(buf, "{ ");
    emit_item(buf, secondary_block, &scope, &exited);
    if (!exited) {
        emit_scope_defers(buf, &scope);
    }
    ncc_buffer_puts(buf, "}");
    dscope_free(&scope);
}

// ----------------------------------------------------------------------------
// Entry: rewrite a function definition whose body uses _defer
// ----------------------------------------------------------------------------

static ncc_parse_tree_t *
xform_defer_funcdef(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *body = ncc_xform_find_child_nt(node, "function_body");
    if (!body) {
        return nullptr;
    }
    ncc_parse_tree_t *compound = ncc_xform_find_child_nt(body,
                                                         "compound_statement");
    if (!compound || !subtree_has_nt(compound, "defer_statement")) {
        return nullptr;
    }

    // v1 limitation: correctly running defers across a goto / labeled
    // break-continue needs label-scope analysis, so reject the combination
    // rather than mis-lower it.
    if (subtree_has_unsupported_jump(compound)) {
        defer_error(compound,
                    "a function using _defer may not (yet) also use goto, "
                    "computed goto, or labeled break/continue");
    }

    // Rewrite the body.
    ncc_buffer_t *body_buf = ncc_buffer_empty();
    emit_compound(body_buf, compound, nullptr, DS_FUNC);
    char *new_body = ncc_buffer_take(body_buf);

    // Rebuild the function definition text, replacing the body verbatim-wise.
    ncc_buffer_t *fbuf = ncc_buffer_empty();
    size_t        nc   = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c == body) {
            ncc_buffer_puts(fbuf, new_body ? new_body : "{ }");
            ncc_buffer_putc(fbuf, ' ');
        }
        else {
            ncc_string_t t = ncc_xform_node_to_text(c);
            ncc_buffer_puts(fbuf, t.data ? t.data : "");
            ncc_buffer_putc(fbuf, ' ');
            if (t.data) {
                ncc_free(t.data);
            }
        }
    }
    ncc_free(new_body);

    char *ftext = ncc_buffer_take(fbuf);
    ncc_parse_tree_t *result = ncc_xform_parse_source(
        ctx->grammar, "function_definition", ftext ? ftext : "", "xform_defer");
    ncc_free(ftext);

    return result;
}

void
ncc_register_defer_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "function_definition", xform_defer_funcdef,
                       "defer");
}
