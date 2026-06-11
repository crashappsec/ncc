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
// Interprocedural signature table: which functions return `?`, and which of
// their parameters are `?`.
// ---------------------------------------------------------------------------

typedef struct {
    char  *name;
    bool   ret_nullable;
    bool  *param_nullable;
    size_t nparams;
} nsig_t;

typedef struct {
    nsig_t *sigs;
    size_t  count;
} nsigtab_t;

static ncc_parse_tree_t *
find_desc(ncc_parse_tree_t *n, const char *name)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return nullptr;
    }
    if (ncc_xform_nt_name_is(n, name)) {
        return n;
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *r = find_desc(ncc_tree_child(n, i), name);
        if (r) {
            return r;
        }
    }
    return nullptr;
}

// Collect the TOP-LEVEL parameter_declarations of a parameter list in order
// (without descending into nested function-pointer parameter lists), recording
// each one's nullability.
static void
collect_params(ncc_parse_tree_t *n, bool **out, size_t *count, size_t *cap)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }
    if (ncc_xform_nt_name_is(n, "parameter_declaration")) {
        // `(void)` parses as a single `void` parameter_declaration but means
        // "no parameters" — skip it.
        ncc_string_t t      = ncc_xform_node_to_text(n);
        bool         isvoid = t.data && strcmp(t.data, "void") == 0;
        if (t.data) {
            ncc_free(t.data);
        }
        if (!isvoid) {
            if (*count >= *cap) {
                *cap = *cap ? *cap * 2 : 4;
                *out = ncc_realloc(*out, *cap * sizeof(bool));
            }
            (*out)[(*count)++] = subtree_has_q(n);
        }
        return; // do not descend into the parameter's own (nested) params
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        collect_params(ncc_tree_child(n, i), out, count, cap);
    }
}

static const nsig_t *
sig_lookup(nsigtab_t *tab, const char *name)
{
    if (!tab || !name) {
        return nullptr;
    }
    for (size_t i = 0; i < tab->count; i++) {
        if (strcmp(tab->sigs[i].name, name) == 0) {
            return &tab->sigs[i];
        }
    }
    return nullptr;
}

// Record the signature of a function declaration / definition.
static void
add_signature(nsigtab_t *tab, ncc_parse_tree_t *node)
{
    // The declarator is a direct child of a function_definition but is nested
    // under init_declarator_list in a prototype declaration; find_desc covers
    // both (it returns the outermost declarator first).
    ncc_parse_tree_t *declr = find_desc(node, "declarator");
    if (!declr) {
        return;
    }
    ncc_parse_tree_t *ptl = find_desc(declr, "parameter_type_list");
    if (!ptl) {
        return; // not a function declarator
    }
    char *name = ncc_layout_declarator_name(declr);
    if (!name) {
        return;
    }
    if (sig_lookup(tab, name)) {
        ncc_free(name); // first declaration wins
        return;
    }

    ncc_parse_tree_t *specs = ncc_xform_find_child_nt(node,
                                                      "declaration_specifiers");

    nsig_t s = {.name = name, .ret_nullable = specs && subtree_has_q(specs)};
    size_t cap = 0;
    collect_params(ptl, &s.param_nullable, &s.nparams, &cap);

    tab->sigs = ncc_realloc(tab->sigs, (tab->count + 1) * sizeof(nsig_t));
    tab->sigs[tab->count++] = s;
}

static void
build_signatures(ncc_parse_tree_t *node, nsigtab_t *tab)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    if (ncc_xform_nt_name_is(node, "function_definition")
        || ncc_xform_nt_name_is(node, "declaration")) {
        add_signature(tab, node);
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        build_signatures(ncc_tree_child(node, i), tab);
    }
}

// ---------------------------------------------------------------------------
// Deref checking
// ---------------------------------------------------------------------------

typedef struct {
    nvars_t   *vars;
    nstate_t  *state;
    nsigtab_t *sigs;
    int        warnings;
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

// Collect the TOP-LEVEL argument expressions of a call's argument list, in
// order (not descending into a nested call's own arguments).
static void
collect_args(ncc_parse_tree_t *n, ncc_parse_tree_t ***out, size_t *count,
             size_t *cap)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }
    if (ncc_xform_nt_name_is(n, "assignment_expression")) {
        if (*count >= *cap) {
            *cap = *cap ? *cap * 2 : 4;
            *out = ncc_realloc(*out, *cap * sizeof(ncc_parse_tree_t *));
        }
        (*out)[(*count)++] = n;
        return;
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        collect_args(ncc_tree_child(n, i), out, count, cap);
    }
}

// A call passing a possibly-null argument to a non-nullable parameter is
// reported.
static void
check_call(ncheck_t *ck, ncc_parse_tree_t *call)
{
    char *callee = expr_ident(ncc_tree_child(call, 0));
    if (!callee) {
        return;
    }
    const nsig_t *sig = sig_lookup(ck->sigs, callee);
    if (!sig) {
        ncc_free(callee);
        return;
    }

    ncc_parse_tree_t *arglist = ncc_xform_find_child_nt(
        call, "argument_expression_list");
    ncc_parse_tree_t **args = nullptr;
    size_t             n    = 0;
    size_t             cap  = 0;
    if (arglist) {
        collect_args(arglist, &args, &n, &cap);
    }

    for (size_t i = 0; i < n && i < sig->nparams; i++) {
        if (sig->param_nullable[i]) {
            continue; // the parameter accepts null
        }
        char *an = expr_ident(args[i]);
        if (an) {
            int ix = nv_index(ck->vars, an);
            if (ix >= 0 && !ck->state->nonnull[ix]) {
                uint32_t line = 0;
                uint32_t col  = 0;
                ncc_xform_first_leaf_pos(args[i], &line, &col);
                fprintf(stderr,
                        "ncc: warning: passing possibly-null '%s' to "
                        "non-nullable parameter %zu of '%s' (line %u, col %u)\n",
                        an, i + 1, callee, line, col);
                ck->warnings++;
            }
            ncc_free(an);
        }
    }

    ncc_free(args);
    ncc_free(callee);
}

// Recursively scan an expression for dereferences of tracked nullable vars:
// `*x`, `x->m`, `x[i]`, and for calls passing nullable args to non-`?` params.
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
            else if (op && strcmp(op, "(") == 0) {
                check_call(ck, node);
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

static ncc_parse_tree_t *unwrap_single(ncc_parse_tree_t *n);

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
            bool handled = false;
            // A call to a known `?`-returning function taints the target as
            // nullable; a call to a known non-`?`-returning function makes it
            // non-null.
            ncc_parse_tree_t *u = unwrap_single(rhs);
            if (u && ncc_xform_nt_name_is(u, "postfix_expression")
                && ncc_tree_num_children(u) >= 2) {
                ncc_parse_tree_t *opn = ncc_tree_child(u, 1);
                if (opn && ncc_tree_is_leaf(opn)
                    && ncc_xform_leaf_text_eq(opn, "(")) {
                    char         *callee = expr_ident(ncc_tree_child(u, 0));
                    const nsig_t *sig = callee ? sig_lookup(ck->sigs, callee)
                                               : nullptr;
                    if (sig) {
                        nn      = !sig->ret_nullable;
                        handled = true;
                    }
                    if (callee) {
                        ncc_free(callee);
                    }
                }
            }
            if (!handled) {
                // Otherwise: an explicit null constant is nullable; everything
                // else (address-of, string, unknown call, ...) is treated as
                // non-null. Erring toward non-null avoids false positives.
                ncc_string_t rt      = ncc_xform_node_to_text(rhs);
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
        // Stop at a postfix_expression (a call/deref/subscript). A
        // unary_expression with an operator (e.g. `!x`) stops via the
        // single-child check below; a 1-child unary wrapper descends into its
        // postfix.
        if (ncc_xform_nt_name_is(n, "postfix_expression")) {
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
analyze_function(ncc_parse_tree_t *func, nsigtab_t *sigs)
{
    nvars_t vars = {0};
    collect_nullable_vars(func, &vars);

    ncc_parse_tree_t *body = ncc_xform_find_child_nt(func, "function_body");
    ncc_parse_tree_t *comp = body ? ncc_xform_find_child_nt(body,
                                                            "compound_statement")
                                  : nullptr;

    // With no `?` variables there is nothing to track for deref warnings, but a
    // call may still pass a non-tracked value — no, only tracked (nullable)
    // values can taint, so an empty tracked set means no possible warning.
    if (vars.count == 0) {
        ncc_free(vars.names);
        return 0;
    }

    int result = 0;
    if (comp) {
        nstate_t state = ns_new(&vars); // all NULLABLE
        ncheck_t ck = {
            .vars = &vars, .state = &state, .sigs = sigs, .warnings = 0};
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
walk_functions(ncc_parse_tree_t *node, nsigtab_t *sigs)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return 0;
    }
    int n = 0;
    if (ncc_xform_nt_name_is(node, "function_definition")) {
        n += analyze_function(node, sigs);
        return n; // no nested functions in C
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        n += walk_functions(ncc_tree_child(node, i), sigs);
    }
    return n;
}

int
ncc_nullability_check(ncc_grammar_t *g, ncc_parse_tree_t *tu, ncc_symtab_t *st)
{
    (void)g;
    (void)st;

    nsigtab_t sigs = {0};
    build_signatures(tu, &sigs);

    int n = walk_functions(tu, &sigs);

    for (size_t i = 0; i < sigs.count; i++) {
        ncc_free(sigs.sigs[i].name);
        ncc_free(sigs.sigs[i].param_nullable);
    }
    ncc_free(sigs.sigs);
    return n;
}
