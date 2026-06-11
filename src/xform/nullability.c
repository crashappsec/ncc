// nullability.c — flow-sensitive null-state analysis for `?` pointers.
//
// Stage 2 (intraprocedural). For each function, the variables declared `?`
// (parameters and locals) are tracked; each is NONNULL or NULLABLE at every
// program point. A NULLABLE value dereferenced (`*p`, `p->m`, `p[i]`) without a
// dominating null check is reported. Null checks refine the state:
//   if (p) { /* p NONNULL here */ }
//   if (!p) { return; } /* p NONNULL after */
// Joins are conservative (NONNULL only if NONNULL on all incoming paths), and
// loop bodies widen any variable they reassign back to NULLABLE.
//
// Reads the `?` markers from the parse tree before xform_nullable strips them.
// Block scoping is approximated by a function-wide tracked set, which is sound
// for the warning (it never suppresses a real unchecked deref).

#include "lib/alloc.h"
#include "parse/nullability.h"
#include "parse/parse_tree.h"
#include "xform/xform_helpers.h"
#include "xform/xform_type_layout.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tracked-variable set + per-point state (parallel bool array; true = NONNULL)
// ---------------------------------------------------------------------------

typedef struct {
    char **names;
    size_t count;
} nvars_t;

typedef struct {
    bool  *nonnull;  // parallel to nvars_t.names
    size_t count;
} nstate_t;

static int
nv_index(nvars_t *v, const char *name)
{
    if (!name) {
        return -1;
    }
    for (size_t i = 0; i < v->count; i++) {
        if (strcmp(v->names[i], name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static nstate_t
ns_new(nvars_t *v)
{
    nstate_t s = {.nonnull = nullptr, .count = v->count};
    if (v->count) {
        s.nonnull = ncc_alloc_array(bool, v->count);
    }
    return s;
}

static nstate_t
ns_copy(nstate_t *s)
{
    nstate_t r = {.nonnull = nullptr, .count = s->count};
    if (s->count) {
        r.nonnull = ncc_alloc_array(bool, s->count);
        memcpy(r.nonnull, s->nonnull, s->count * sizeof(bool));
    }
    return r;
}

static void
ns_free(nstate_t *s)
{
    ncc_free(s->nonnull);
    s->nonnull = nullptr;
}

// dst := dst AND other (a NONNULL survives a join only if NONNULL on both).
static void
ns_join_into(nstate_t *dst, nstate_t *other)
{
    for (size_t i = 0; i < dst->count && i < other->count; i++) {
        dst->nonnull[i] = dst->nonnull[i] && other->nonnull[i];
    }
}

// ---------------------------------------------------------------------------
// Small tree predicates
// ---------------------------------------------------------------------------

// A non-leaf whose entire text is "?" — the nullable qualifier (not the ternary
// `?`, which is a bare leaf).
static bool
node_is_q(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    ncc_string_t t = ncc_xform_node_to_text(node);
    bool         m = t.data && strcmp(t.data, "?") == 0;
    if (t.data) {
        ncc_free(t.data);
    }
    return m;
}

static bool
subtree_has_q(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    if (node_is_q(node)) {
        return true;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_q(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

// If `node` reduces to a lone identifier, return that name (caller frees);
// else nullptr. (Parenthesized forms are not unwrapped — conservative.)
static char *
expr_ident(ncc_parse_tree_t *node)
{
    ncc_string_t t = ncc_xform_node_to_text(node);
    if (!t.data) {
        return nullptr;
    }
    const char *s = t.data;
    while (*s == ' ') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        len--;
    }
    bool ident = len > 0 && (isalpha((unsigned char)s[0]) || s[0] == '_');
    for (size_t i = 0; ident && i < len; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            ident = false;
        }
    }
    char *r = nullptr;
    if (ident) {
        r = ncc_alloc_array(char, len + 1);
        memcpy(r, s, len);
        r[len] = '\0';
    }
    ncc_free(t.data);
    return r;
}

// ---------------------------------------------------------------------------
// Collect the function's `?` variables (parameters + locals)
// ---------------------------------------------------------------------------

static void
collect_decl_names(ncc_parse_tree_t *decl, nvars_t *out)
{
    // A declaration / parameter_declaration whose type carries `?`: record each
    // declared name.
    if (!subtree_has_q(decl)) {
        return;
    }
    size_t nc = ncc_tree_num_children(decl);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c    = ncc_tree_child(decl, i);
        char             *name = ncc_layout_declarator_name(c);
        if (name) {
            if (nv_index(out, name) < 0) {
                out->names = ncc_realloc(out->names,
                                         (out->count + 1) * sizeof(char *));
                out->names[out->count++] = name;
            }
            else {
                ncc_free(name);
            }
        }
    }
}

static void
collect_nullable_vars(ncc_parse_tree_t *node, nvars_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    if (ncc_xform_nt_name_is(node, "declaration")
        || ncc_xform_nt_name_is(node, "parameter_declaration")) {
        collect_decl_names(node, out);
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_nullable_vars(ncc_tree_child(node, i), out);
    }
}

// ---------------------------------------------------------------------------
// Deref checking
// ---------------------------------------------------------------------------

typedef struct {
    nvars_t  *vars;
    nstate_t *state;
    int       warnings;
} ncheck_t;

static void
warn_deref(ncheck_t *ck, ncc_parse_tree_t *at, const char *name)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(at, &line, &col);
    fprintf(stderr,
            "ncc: warning: possible null dereference of nullable '%s' "
            "(line %u, col %u)\n",
            name, line, col);
    ck->warnings++;
}

// Warn if `base` is a tracked variable currently NULLABLE.
static void
check_base(ncheck_t *ck, ncc_parse_tree_t *base, ncc_parse_tree_t *at)
{
    char *name = expr_ident(base);
    if (!name) {
        return;
    }
    int ix = nv_index(ck->vars, name);
    if (ix >= 0 && !ck->state->nonnull[ix]) {
        warn_deref(ck, at, name);
    }
    ncc_free(name);
}

// Recursively scan an expression for dereferences of tracked nullable vars:
// `*x`, `x->m`, `x[i]`.
static void
check_derefs(ncheck_t *ck, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);

    if (ncc_xform_nt_name_is(node, "unary_expression") && nc >= 2) {
        // <unary_operator> <cast_expression> — the `*` deref.
        ncc_parse_tree_t *op = ncc_tree_child(node, 0);
        ncc_string_t      ot = ncc_xform_node_to_text(op);
        bool              is_star = ot.data && strcmp(ot.data, "*") == 0;
        if (ot.data) {
            ncc_free(ot.data);
        }
        if (is_star) {
            check_base(ck, ncc_tree_child(node, 1), node);
        }
    }
    else if (ncc_xform_nt_name_is(node, "postfix_expression") && nc >= 2) {
        // <postfix> "->" id   |   <postfix> "[" expr "]"
        ncc_parse_tree_t *opn = ncc_tree_child(node, 1);
        if (opn && ncc_tree_is_leaf(opn)) {
            const char *op = ncc_xform_leaf_text(opn);
            if (op && (strcmp(op, "->") == 0 || strcmp(op, "[") == 0)) {
                check_base(ck, ncc_tree_child(node, 0), node);
            }
        }
    }

    for (size_t i = 0; i < nc; i++) {
        check_derefs(ck, ncc_tree_child(node, i));
    }
}

// ---------------------------------------------------------------------------
// Statement walk with state threading
// ---------------------------------------------------------------------------

static bool stmt_exits(ncc_parse_tree_t *node);

static void analyze_stmt(ncheck_t *ck, ncc_parse_tree_t *node);

// Set the nonnull bit of a tracked var.
static void
set_nonnull(ncheck_t *ck, const char *name, bool nn)
{
    int ix = nv_index(ck->vars, name);
    if (ix >= 0) {
        ck->state->nonnull[ix] = nn;
    }
}

// Apply a top-level `x = e;` assignment effect: x becomes NONNULL only if e is
// obviously non-null (address-of or string literal); otherwise NULLABLE.
static void
apply_assignment(ncheck_t *ck, ncc_parse_tree_t *node)
{
    if (!ncc_xform_nt_name_is(node, "assignment_expression")) {
        return;
    }
    size_t nc = ncc_tree_num_children(node);
    if (nc < 3) {
        return;
    }
    ncc_parse_tree_t *opn = ncc_tree_child(node, 1);
    // The operator is an <assignment_operator> node (text "="), not a bare leaf.
    ncc_string_t      ot   = ncc_xform_node_to_text(opn);
    bool              eq   = ot.data && strcmp(ot.data, "=") == 0;
    if (ot.data) {
        ncc_free(ot.data);
    }
    if (!eq) {
        return; // only a plain `=` reassigns nullability
    }
    char *lhs = expr_ident(ncc_tree_child(node, 0));
    if (!lhs) {
        return;
    }
    if (nv_index(ck->vars, lhs) >= 0) {
        ncc_parse_tree_t *rhs  = ncc_tree_child(node, 2);
        char             *rid  = expr_ident(rhs);
        bool              nn;
        if (rid) {
            // Assigning another pointer: nullable only if the source is a
            // currently-NULLABLE tracked variable.
            int rix = nv_index(ck->vars, rid);
            nn = !(rix >= 0 && !ck->state->nonnull[rix]);
            ncc_free(rid);
        }
        else {
            // A complex RHS: treat an explicit null constant as nullable,
            // everything else (address-of, string, call, ...) as non-null.
            // Erring toward non-null avoids false positives; interprocedural
            // call results are handled in a later stage.
            ncc_string_t rt = ncc_xform_node_to_text(rhs);
            bool         is_null = false;
            if (rt.data) {
                const char *r = rt.data;
                while (*r == ' ') {
                    r++;
                }
                is_null = strcmp(r, "0") == 0 || strcmp(r, "nullptr") == 0;
                ncc_free(rt.data);
            }
            nn = !is_null;
        }
        set_nonnull(ck, lhs, nn);
    }
    ncc_free(lhs);
}

// Find a top-level assignment_expression in an expression statement subtree.
static ncc_parse_tree_t *
find_assignment(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }
    if (ncc_xform_nt_name_is(node, "assignment_expression")
        && ncc_tree_num_children(node) >= 3) {
        return node;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *r = find_assignment(ncc_tree_child(node, i));
        if (r) {
            return r;
        }
    }
    return nullptr;
}

// Descend through single-meaningful-child wrappers (selection_header,
// expression, assignment_expression, ...) until a node that branches or is a
// unary/postfix expression — so a wrapped `!p` or `p` is reachable.
static ncc_parse_tree_t *
unwrap_single(ncc_parse_tree_t *n)
{
    while (n && !ncc_tree_is_leaf(n)) {
        if (ncc_xform_nt_name_is(n, "unary_expression")
            || ncc_xform_nt_name_is(n, "postfix_expression")) {
            break;
        }
        ncc_parse_tree_t *only  = nullptr;
        int               count = 0;
        size_t            nc     = ncc_tree_num_children(n);
        for (size_t i = 0; i < nc; i++) {
            ncc_parse_tree_t *c = ncc_tree_child(n, i);
            if (c && !ncc_tree_is_leaf(c)) {
                count++;
                only = c;
            }
        }
        if (count == 1) {
            n = only;
            continue;
        }
        break;
    }
    return n;
}

// Condition refinement: if the controlling expression is `x` or `!x`, return
// the variable name and whether truth implies x is NONNULL.
static char *
cond_var(ncc_parse_tree_t *cond, bool *true_means_nonnull)
{
    cond = unwrap_single(cond);
    // `!x` → unary_expression with leading "!"
    if (ncc_xform_nt_name_is(cond, "unary_expression")
        && ncc_tree_num_children(cond) >= 2) {
        ncc_parse_tree_t *op = ncc_tree_child(cond, 0);
        ncc_string_t      ot = ncc_xform_node_to_text(op);
        bool              is_not = ot.data && strcmp(ot.data, "!") == 0;
        if (ot.data) {
            ncc_free(ot.data);
        }
        if (is_not) {
            char *v = expr_ident(ncc_tree_child(cond, 1));
            if (v) {
                *true_means_nonnull = false; // !x true => x is null
                return v;
            }
        }
    }
    char *v = expr_ident(cond);
    if (v) {
        *true_means_nonnull = true; // x true => x nonnull
        return v;
    }
    return nullptr;
}

// Names assigned anywhere in a subtree (for loop widening). Appends to out.
static void
collect_assigned(ncc_parse_tree_t *node, nvars_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    ncc_parse_tree_t *a = nullptr;
    if (ncc_xform_nt_name_is(node, "assignment_expression")
        && ncc_tree_num_children(node) >= 3) {
        a = node;
    }
    if (a) {
        ncc_parse_tree_t *opn = ncc_tree_child(a, 1);
        ncc_string_t      ot  = ncc_xform_node_to_text(opn);
        bool              eq  = ot.data && strcmp(ot.data, "=") == 0;
        if (ot.data) {
            ncc_free(ot.data);
        }
        if (eq) {
            char *n = expr_ident(ncc_tree_child(a, 0));
            if (n && nv_index(out, n) < 0) {
                out->names = ncc_realloc(out->names,
                                         (out->count + 1) * sizeof(char *));
                out->names[out->count++] = n;
            }
            else {
                ncc_free(n);
            }
        }
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_assigned(ncc_tree_child(node, i), out);
    }
}

static ncc_parse_tree_t *
find_secondary_block(ncc_parse_tree_t *node, int which)
{
    // Return the `which`-th (0-based) secondary_block child.
    size_t nc    = ncc_tree_num_children(node);
    int    seen  = 0;
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c && !ncc_tree_is_leaf(c)
            && ncc_xform_nt_name_is(c, "secondary_block")) {
            if (seen == which) {
                return c;
            }
            seen++;
        }
    }
    return nullptr;
}

static void
analyze_if(ncheck_t *ck, ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *cond = ncc_xform_find_child_nt(node, "selection_header");
    if (!cond) {
        cond = ncc_xform_find_child_nt(node, "expression");
    }
    if (cond) {
        check_derefs(ck, cond);
    }

    bool  truth_nonnull = false;
    char *cv = cond ? cond_var(cond, &truth_nonnull) : nullptr;

    ncc_parse_tree_t *then_blk = find_secondary_block(node, 0);
    ncc_parse_tree_t *else_blk = find_secondary_block(node, 1);

    nstate_t entry = ns_copy(ck->state);

    // then-branch: refine cv per the condition's true sense.
    nstate_t then_state = ns_copy(&entry);
    if (cv) {
        int ix = nv_index(ck->vars, cv);
        if (ix >= 0 && truth_nonnull) {
            then_state.nonnull[ix] = true;
        }
    }
    nstate_t saved = *ck->state;
    *ck->state = then_state;
    if (then_blk) {
        analyze_stmt(ck, then_blk);
    }
    then_state = *ck->state;

    // else-branch: refine cv per the condition's false sense.
    nstate_t else_state = ns_copy(&entry);
    if (cv) {
        int ix = nv_index(ck->vars, cv);
        if (ix >= 0 && !truth_nonnull) {
            else_state.nonnull[ix] = true; // !x false => x nonnull
        }
    }
    *ck->state = else_state;
    if (else_blk) {
        analyze_stmt(ck, else_blk);
    }
    else_state = *ck->state;

    bool then_exits = then_blk && stmt_exits(then_blk);
    bool else_exits = else_blk && stmt_exits(else_blk);

    // Merge.
    nstate_t merged;
    if (then_exits && !else_exits) {
        merged = ns_copy(&else_state);
    }
    else if (else_exits && !then_exits) {
        merged = ns_copy(&then_state);
    }
    else {
        merged = ns_copy(&then_state);
        ns_join_into(&merged, &else_state);
    }

    ns_free(&then_state);
    ns_free(&else_state);
    ns_free(&entry);
    ns_free(&saved);
    *ck->state = merged;
    if (cv) {
        ncc_free(cv);
    }
}

static void
analyze_loop(ncheck_t *ck, ncc_parse_tree_t *node)
{
    // Body is the (last) secondary_block.
    ncc_parse_tree_t *body = nullptr;
    size_t            nc   = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c && !ncc_tree_is_leaf(c)
            && ncc_xform_nt_name_is(c, "secondary_block")) {
            body = c;
        }
    }

    // Check derefs in the loop header expressions.
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c && !ncc_tree_is_leaf(c)
            && !ncc_xform_nt_name_is(c, "secondary_block")) {
            check_derefs(ck, c);
        }
    }

    if (!body) {
        return;
    }

    // Widen: any tracked var the body reassigns may be NULLABLE on a back edge.
    nvars_t assigned = {0};
    collect_assigned(body, &assigned);
    for (size_t i = 0; i < assigned.count; i++) {
        set_nonnull(ck, assigned.names[i], false);
        ncc_free(assigned.names[i]);
    }
    ncc_free(assigned.names);

    // A `while (x)` / `for (...; x; ...)` guard makes x NONNULL in the body.
    ncc_parse_tree_t *guard = ncc_xform_find_child_nt(node, "expression");
    if (guard) {
        bool  tn = false;
        char *v  = cond_var(guard, &tn);
        if (v && tn) {
            set_nonnull(ck, v, true);
        }
        if (v) {
            ncc_free(v);
        }
    }

    analyze_stmt(ck, body);
}

// True if the statement unconditionally transfers control out (return / break /
// continue / goto) as its final action.
static bool
stmt_exits(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        return false;
    }
    if (ncc_xform_nt_name_is(node, "jump_statement")) {
        return true;
    }
    // For a block, the last meaningful statement decides.
    if (ncc_xform_nt_name_is(node, "compound_statement")) {
        ncc_parse_tree_t *list = ncc_xform_find_child_nt(node,
                                                         "block_item_list");
        if (!list) {
            return false;
        }
        size_t nc = ncc_tree_num_children(list);
        for (size_t i = nc; i-- > 0;) {
            ncc_parse_tree_t *c = ncc_tree_child(list, i);
            if (c && !ncc_tree_is_leaf(c)) {
                return stmt_exits(c);
            }
        }
        return false;
    }
    // Descend through wrappers (secondary_block, statement, unlabeled_statement
    // with an optional leading attribute slot, primary_block, group nodes) into
    // the LAST non-leaf child — the effective statement comes last.
    size_t            nc   = ncc_tree_num_children(node);
    ncc_parse_tree_t *last = nullptr;
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c && !ncc_tree_is_leaf(c)) {
            last = c;
        }
    }
    return last ? stmt_exits(last) : false;
}

static void
analyze_stmt(ncheck_t *ck, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "selection_statement")) {
        const char *kw = ncc_xform_get_first_leaf_text(node);
        if (kw && strcmp(kw, "if") == 0) {
            analyze_if(ck, node);
            return;
        }
        // switch: check derefs, analyze body without refinement.
        check_derefs(ck, node);
        return;
    }

    if (ncc_xform_nt_name_is(node, "iteration_statement")) {
        analyze_loop(ck, node);
        return;
    }

    if (ncc_xform_nt_name_is(node, "expression_statement")
        || ncc_xform_nt_name_is(node, "declaration")) {
        check_derefs(ck, node);
        ncc_parse_tree_t *a = find_assignment(node);
        if (a) {
            apply_assignment(ck, a);
        }
        return;
    }

    if (ncc_xform_nt_name_is(node, "jump_statement")) {
        check_derefs(ck, node);
        return;
    }

    // compound_statement / wrappers: recurse in order, threading state.
    if (ncc_xform_nt_name_is(node, "compound_statement")) {
        ncc_parse_tree_t *list = ncc_xform_find_child_nt(node,
                                                         "block_item_list");
        if (list) {
            size_t nc = ncc_tree_num_children(list);
            for (size_t i = 0; i < nc; i++) {
                analyze_stmt(ck, ncc_tree_child(list, i));
            }
        }
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        analyze_stmt(ck, ncc_tree_child(node, i));
    }
}

// ---------------------------------------------------------------------------
// Per-function driver
// ---------------------------------------------------------------------------

static int
analyze_function(ncc_parse_tree_t *func)
{
    nvars_t vars = {0};
    collect_nullable_vars(func, &vars);
    if (vars.count == 0) {
        return 0;
    }

    ncc_parse_tree_t *body = ncc_xform_find_child_nt(func, "function_body");
    ncc_parse_tree_t *comp = body ? ncc_xform_find_child_nt(body,
                                                            "compound_statement")
                                  : nullptr;

    int result = 0;
    if (comp) {
        nstate_t state = ns_new(&vars); // all NULLABLE
        ncheck_t ck    = {.vars = &vars, .state = &state, .warnings = 0};
        analyze_stmt(&ck, comp);
        result = ck.warnings;
        ns_free(&state);
    }

    for (size_t i = 0; i < vars.count; i++) {
        ncc_free(vars.names[i]);
    }
    ncc_free(vars.names);
    return result;
}

static int
walk_functions(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return 0;
    }
    int n = 0;
    if (ncc_xform_nt_name_is(node, "function_definition")) {
        n += analyze_function(node);
        return n; // no nested functions in C
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        n += walk_functions(ncc_tree_child(node, i));
    }
    return n;
}

int
ncc_nullability_check(ncc_grammar_t *g, ncc_parse_tree_t *tu, ncc_symtab_t *st)
{
    (void)g;
    (void)st;
    return walk_functions(tu);
}
