// nodiscard.c — must-check result returns ([[nodiscard]] for result types).
//
// A function returning a result-protocol aggregate (one with both an `is_ok`
// and an `err` member — n00b_result_t(T)) advertises a value the caller must
// inspect. Calling it as a bare expression statement (`f(x);`) discards the
// result, dropping the error. This pass reports that; the opt-out is an
// explicit void cast (`(void)f(x);`).
//
// Read-only: emits warnings, never transforms the tree. Runs after transforms,
// so `_generic_struct` result types are lowered to concrete `struct __<tag>`
// resolvable through the (rebuilt) symbol table, and `_try` — which consumes a
// result — is already lowered to a statement expression rather than a call.

#include "lib/alloc.h"
#include "parse/nodiscard.h"
#include "parse/parse_tree.h"
#include "parse/type_infer.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <string.h>

static bool
is_ident_text(const char *t)
{
    if (!t || !((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z')
                || t[0] == '_')) {
        return false;
    }
    for (const char *p = t; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
              || (*p >= '0' && *p <= '9') || *p == '_')) {
            return false;
        }
    }
    return true;
}

// The last identifier-form leaf token in a subtree. A member's declared name is
// the last identifier before its `;` regardless of whether the name parsed as a
// declarator or got mis-read as a typedef-name type specifier (the soft
// identifier ambiguity), so this is robust to that mis-parse.
static void
last_ident(ncc_parse_tree_t *n, const char **out)
{
    if (!n) {
        return;
    }
    if (ncc_tree_is_leaf(n)) {
        const char *t = ncc_xform_leaf_text(n);
        if (is_ident_text(t)) {
            *out = t;
        }
        return;
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        last_ident(ncc_tree_child(n, i), out);
    }
}

// Does this aggregate specifier carry the result protocol: a member named
// `is_ok` and a member named `err`? Scans only the aggregate's own members (it
// does not descend into a nested aggregate, whose members are not ours).
static void
scan_result_members(ncc_parse_tree_t *n, bool top, bool *has_is_ok,
                    bool *has_err)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }
    if (!top
        && (ncc_xform_nt_name_is(n, "struct_or_union_specifier")
            || ncc_xform_nt_name_is(n, "enum_specifier"))) {
        return; // a nested aggregate's members are not this type's members
    }
    if (ncc_xform_nt_name_is(n, "member_declaration")) {
        const char *name = nullptr;
        last_ident(n, &name);
        if (name) {
            if (strcmp(name, "is_ok") == 0) {
                *has_is_ok = true;
            }
            else if (strcmp(name, "err") == 0) {
                *has_err = true;
            }
        }
        return; // one member line — its type body (if any) is not our protocol
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        scan_result_members(ncc_tree_child(n, i), false, has_is_ok, has_err);
    }
}

static bool
is_result_spec(ncc_parse_tree_t *spec)
{
    bool has_is_ok = false;
    bool has_err   = false;
    scan_result_members(spec, true, &has_is_ok, &has_err);
    return has_is_ok && has_err;
}

// First struct/union specifier in a subtree (e.g. an inline `_generic_struct`
// return type carried in a function's declaration_specifiers).
static ncc_parse_tree_t *
find_struct_spec(ncc_parse_tree_t *n)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return nullptr;
    }
    if (ncc_xform_nt_name_is(n, "struct_or_union_specifier")) {
        return n;
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *r = find_struct_spec(ncc_tree_child(n, i));
        if (r) {
            return r;
        }
    }
    return nullptr;
}

// Descend through single-meaningful-child wrappers until a node that branches
// or is a postfix expression (a call/deref/subscript) — or a try_expression,
// which consumes the result it is applied to.
static ncc_parse_tree_t *
unwrap(ncc_parse_tree_t *n)
{
    while (n && !ncc_tree_is_leaf(n)) {
        if (ncc_xform_nt_name_is(n, "postfix_expression")
            || ncc_xform_nt_name_is(n, "try_expression")) {
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

// The lone identifier a node reduces to (a callee name), or nullptr.
static char *
callee_name(ncc_parse_tree_t *n)
{
    ncc_string_t t = ncc_xform_node_to_text(n);
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
    bool ident = len > 0 && (((s[0] >= 'a' && s[0] <= 'z')
                              || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_'));
    for (size_t i = 0; ident && i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_')) {
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

// The result-protocol aggregate a call returns by value, or nullptr. A pointer
// return is not a by-value result (you may legitimately hold the pointer).
static ncc_parse_tree_t *
call_result_spec(ncc_symtab_t *st, ncc_parse_tree_t *call)
{
    char *rt       = ncc_type_of_expr(st, call);
    bool  has_star = rt && strchr(rt, '*') != nullptr;
    // A typedef'd / named result resolves through the symbol table.
    ncc_parse_tree_t *spec = (rt && !has_star)
                                 ? ncc_symtab_aggregate_spec(st, rt)
                                 : nullptr;
    if (rt) {
        ncc_free(rt);
    }
    if (has_star) {
        return nullptr;
    }
    if (spec) {
        return spec;
    }
    // Inline `_generic_struct` / struct return: the spelling isn't a resolvable
    // type name, so scan the callee's stored return-type specifiers directly.
    char *callee = callee_name(ncc_tree_child(call, 0));
    if (!callee) {
        return nullptr;
    }
    ncc_sym_entry_t *sym = ncc_symtab_lookup(st, ncc_string_empty(),
                                             ncc_string_from_cstr(callee));
    ncc_free(callee);
    if (sym && sym->kind == NCC_SYM_FUNCTION && sym->type_node) {
        return find_struct_spec(sym->type_node);
    }
    return nullptr;
}

static void
check_stmt(ncc_symtab_t *st, ncc_parse_tree_t *n, int *warnings)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }

    if (ncc_xform_nt_name_is(n, "expression_statement")) {
        // The statement's expression is its first non-leaf, non-attribute child.
        ncc_parse_tree_t *expr = nullptr;
        size_t            nc   = ncc_tree_num_children(n);
        for (size_t i = 0; i < nc; i++) {
            ncc_parse_tree_t *c = ncc_tree_child(n, i);
            if (c && !ncc_tree_is_leaf(c)
                && !ncc_xform_nt_name_is(c, "attribute_specifier_sequence")) {
                expr = c;
                break;
            }
        }

        ncc_parse_tree_t *u = expr ? unwrap(expr) : nullptr;
        // A discarded result requires a bare call: `f(...)` as the whole
        // statement. `(void)f()` unwraps to a cast_expression (not a call) and
        // is the intended opt-out; `x = f()` to an assignment; `_try f()` to a
        // try_expression — none of which warn.
        if (u && ncc_xform_nt_name_is(u, "postfix_expression")
            && ncc_tree_num_children(u) >= 2) {
            ncc_parse_tree_t *opn = ncc_tree_child(u, 1);
            if (opn && ncc_tree_is_leaf(opn)
                && ncc_xform_leaf_text_eq(opn, "(")) {
                ncc_parse_tree_t *spec = call_result_spec(st, u);
                if (spec && is_result_spec(spec)) {
                    char    *callee = callee_name(ncc_tree_child(u, 0));
                    uint32_t line   = 0;
                    uint32_t col    = 0;
                    ncc_xform_first_leaf_pos(u, &line, &col);
                    fprintf(stderr,
                            "ncc: warning: result of '%s()' is ignored; a "
                            "result must be checked (cast to (void) to "
                            "discard) (line %u, col %u)\n",
                            callee ? callee : "call", line, col);
                    (*warnings)++;
                    if (callee) {
                        ncc_free(callee);
                    }
                }
            }
        }
        return; // no nested statements inside an expression statement
    }

    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        check_stmt(st, ncc_tree_child(n, i), warnings);
    }
}

int
ncc_nodiscard_check(ncc_grammar_t *g, ncc_parse_tree_t *tu, ncc_symtab_t *st)
{
    (void)g;
    if (!tu || !st) {
        return 0;
    }
    int warnings = 0;
    check_stmt(st, tu, &warnings);
    return warnings;
}
