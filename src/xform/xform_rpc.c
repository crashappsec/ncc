// xform_rpc.c — Transform: `@rpc("svc/method")` function annotation.
//
// Recognizes `@rpc(...)` on a function declaration or definition.
//
// Phase B scope: find @rpc clauses on external_declarations, validate
// the method-string format, reject combination with `_kargs`, and
// strip the clause from the parent so plain clang accepts the result.
// The synthesized dispatcher / ctor / client stub land in subsequent
// phases.
//
// Registered pre-order on "translation_unit". The framework continues
// recursing into children after we return, so later xforms still see
// the (now-clause-free) declarations.

#include "lib/alloc.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_rpc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Method-string validator
//
// Regex equivalent (per spec §5.3):
//   ^[a-zA-Z_][a-zA-Z0-9_.]*\.[A-Z][a-zA-Z0-9]*\/[A-Z][a-zA-Z0-9]*$
//
// Hand-rolled to avoid pulling in a regex dependency for one
// validation site.
// ============================================================================

static bool
is_lower_alpha_digit_under(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool
is_alpha_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}

static bool
validate_method_string(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    const char *slash = strchr(s, '/');

    if (!slash || strchr(slash + 1, '/')) {
        return false;
    }

    // Method (after slash): [A-Z][a-zA-Z0-9]*
    const char *m = slash + 1;

    if (*m < 'A' || *m > 'Z') {
        return false;
    }

    for (m++; *m; m++) {
        if (!is_alpha_digit(*m)) {
            return false;
        }
    }

    // Service path (before slash): identifier chars / dots, at least
    // one dot, and the segment after the last dot must start with an
    // uppercase letter.
    size_t pre_len = (size_t)(slash - s);

    if (pre_len == 0) {
        return false;
    }

    if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z')
          || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }

    const char *last_dot = nullptr;

    for (size_t i = 0; i < pre_len; i++) {
        char c = s[i];

        if (c == '.') {
            last_dot = s + i;
        }
        else if (!is_lower_alpha_digit_under(c)) {
            return false;
        }
    }

    if (!last_dot) {
        return false;
    }

    const char *svc = last_dot + 1;

    if (svc >= slash) {
        return false;
    }

    if (*svc < 'A' || *svc > 'Z') {
        return false;
    }

    for (svc++; svc < slash; svc++) {
        if (!is_alpha_digit(*svc)) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Extract the method string from an <rpc_clause> subtree.
//
// Grammar: <rpc_clause> ::= %"@" %"rpc" %"(" <string_literal> %")"
// <string_literal> ::= %STRING+   — each STRING leaf carries its own
// surrounding quotes; for adjacent string literals we concatenate
// their contents.
// ============================================================================

static char *
extract_method_string(ncc_parse_tree_t *rpc_clause)
{
    ncc_parse_tree_t *str_lit
        = ncc_xform_find_child_nt(rpc_clause, "string_literal");

    if (!str_lit) {
        return nullptr;
    }

    ncc_string_t text = ncc_xform_node_to_text(str_lit);

    if (!text.data) {
        return nullptr;
    }

    // Strip outer quotes (and any whitespace) for each STRING piece.
    // Walks the verbatim text and copies bytes that lie strictly
    // between matched quote pairs.
    char *out  = ncc_alloc_size(1, text.u8_bytes + 1);
    size_t  oi = 0;
    bool    in = false;

    for (size_t i = 0; i < text.u8_bytes; i++) {
        char c = text.data[i];

        if (c == '"') {
            in = !in;
            continue;
        }

        if (!in) {
            continue;
        }

        if (c == '\\' && i + 1 < text.u8_bytes) {
            // Preserve backslash escapes verbatim — the method
            // grammar doesn't allow them, but we'd rather hand the
            // text to the validator and produce a precise error than
            // strip them into something unrecognizable.
            out[oi++] = text.data[i++];
            out[oi++] = text.data[i];
            continue;
        }

        out[oi++] = c;
    }

    out[oi] = '\0';
    ncc_free(text.data);
    return out;
}

// ============================================================================
// Find an <rpc_clause> or <keyword_clause> child of a
// function_definition / declaration.
// ============================================================================

static ncc_parse_tree_t *
find_child_nt_index(ncc_parse_tree_t *parent, const char *name, size_t *idx)
{
    size_t nc = ncc_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(parent, i);

        if (c && !ncc_tree_is_leaf(c) && ncc_xform_nt_name_is(c, name)) {
            if (idx) {
                *idx = i;
            }
            return c;
        }
    }

    return nullptr;
}

// ============================================================================
// Diagnostics
// ============================================================================

[[noreturn]] static void
rpc_diagnostic(ncc_parse_tree_t *anchor, const char *fmt, ...)
{
    uint32_t line = 0, col = 0;

    if (anchor) {
        ncc_xform_first_leaf_pos(anchor, &line, &col);
    }

    fprintf(stderr, "ncc: error: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, " (line %u, col %u)\n", line, col);
    exit(1);
}

// ============================================================================
// Process one external_declaration's inner (function_definition or
// declaration).
//
// Returns true if the inner had an @rpc clause we recognized (and
// stripped); false otherwise.
// ============================================================================

static bool
process_inner(ncc_parse_tree_t *inner)
{
    size_t            rpc_idx;
    ncc_parse_tree_t *rpc_clause = find_child_nt_index(inner, "rpc_clause",
                                                       &rpc_idx);

    if (!rpc_clause) {
        return false;
    }

    // Spec §4: `@rpc` and `_kargs` on the same function are rejected
    // at parse time. The grammar accepts the combination so we can
    // produce a precise diagnostic instead of a generic parse fail.
    if (find_child_nt_index(inner, "keyword_clause", nullptr)) {
        rpc_diagnostic(rpc_clause,
                       "@rpc cannot be combined with _kargs on the same "
                       "function");
    }

    char *method = extract_method_string(rpc_clause);

    if (!method) {
        rpc_diagnostic(rpc_clause,
                       "@rpc requires a string literal of the form "
                       "\"package.Service/Method\"");
    }

    if (!validate_method_string(method)) {
        rpc_diagnostic(rpc_clause,
                       "@rpc method string \"%s\" does not match "
                       "\"<package>.<Service>/<Method>\" (package: "
                       "dotted identifiers; Service and Method must "
                       "start with an uppercase letter)",
                       method);
    }

    ncc_free(method);

    // Strip the rpc_clause so plain clang accepts the result. The
    // emit phase reads what's left in the tree; once the clause is
    // gone, the function definition / prototype looks like vanilla C.
    ncc_xform_remove_child(inner, rpc_idx);

    return true;
}

// ============================================================================
// Recursive walker: process every function_definition / declaration
// anywhere in the tree.
//
// The parser groups <external_declaration>+ under a `$$group_*`
// wrapper, so a flat TU-children loop would miss them. Recursion
// also handles the (legal-but-unusual) case of a declaration with
// @rpc nested inside a function body. We recurse into children
// first, then process this node — that way `process_inner`'s child
// removal can't perturb an in-flight loop.
// ============================================================================

static bool
walk_and_strip(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    bool changed = false;

    size_t nc = ncc_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        if (walk_and_strip(ncc_tree_child(node, i))) {
            changed = true;
        }
    }

    if (ncc_xform_nt_name_is(node, "function_definition")
        || ncc_xform_nt_name_is(node, "declaration")) {
        if (process_inner(node)) {
            changed = true;
        }
    }

    return changed;
}

// ============================================================================
// Pre-order transform on translation_unit
// ============================================================================

static ncc_parse_tree_t *
xform_rpc_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
             [[maybe_unused]] ncc_xform_control_t *control)
{
    if (!walk_and_strip(tu)) {
        return nullptr;
    }

    ctx->nodes_replaced++;
    return tu;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_register_rpc_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "translation_unit", xform_rpc_tu, "rpc");
}
