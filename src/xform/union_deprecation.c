// union_deprecation.c -- warn (or error) on traditional C unions, steering
// them toward n00b_variant_t. See include/parse/union_deprecation.h.
//
// Read-only: emits diagnostics, never transforms the tree. Runs PRE-transform
// (before _generic_struct lowering), so n00b_variant_t value-unions are still
// in their source `_generic_struct typeid("n00b_variant", ...)` form and are
// identified by that marker — not by member names (which fold ambiguously) and
// not after lowering (which produces synthesized, file-less, parent-less nodes
// that no type-model query can see).

#include "ncc.h"
#include "parse/union_deprecation.h"
#include "parse/token.h"
#include "xform/xform_helpers.h"
#include "xform/xform_type_layout.h"
#include "lib/option.h"
#include "lib/tree.h"

#include <stdio.h>
#include <string.h>

// First-leaf source location of `node`: returns the file path (or nullptr) and
// fills line/col, so the diagnostic can name the header a union lives in.
static const char *
first_leaf_loc(ncc_parse_tree_t *node, uint32_t *line, uint32_t *col)
{
    if (!node) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok) {
            *line = tok->line;
            *col  = tok->column;
            return ncc_option_is_set(tok->file) ? ncc_option_get(tok->file).data
                                                : nullptr;
        }
        return nullptr;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        const char *f = first_leaf_loc(ncc_tree_child(node, i), line, col);
        if (*line != 0) {
            return f;
        }
    }
    return nullptr;
}

// Is `su` the n00b_variant_t shape: a `_generic_struct` (child 0 is the
// `_kw_generic_struct` keyword) whose tag is `typeid("n00b_variant", ...)`?
// That marker is unique to the n00b_variant_t macro, so the struct's value
// union is a precisely-scanned variant alternative table, not a raw union.
static bool
is_n00b_variant_generic_struct(ncc_parse_tree_t *su)
{
    if (ncc_tree_num_children(su) == 0) {
        return false;
    }
    ncc_parse_tree_t *kw = ncc_tree_child(su, 0);
    if (!kw || !ncc_xform_nt_name_is(kw, "_kw_generic_struct")) {
        return false;
    }
    ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
    if (!tag) {
        return false;
    }
    char *text = ncc_layout_node_text(tag);
    bool  ok   = text != nullptr && strstr(text, "n00b_variant") != nullptr;
    if (text) {
        ncc_free(text);
    }
    return ok;
}

// Is this union the value-union of an n00b_variant_t? Walk up to the nearest
// enclosing aggregate specifier and test the marker. Pre-transform, parent
// pointers are set and the _generic_struct is intact, so this is exact: a bare
// union nested in a normal struct (or in a variant arm, whose nearest enclosing
// struct is the arm, not the variant) is correctly NOT excluded.
static bool
is_variant_value_union(ncc_parse_tree_t *union_node)
{
    ncc_parse_tree_t *cur = union_node;
    while (cur && !ncc_tree_is_leaf(cur)) {
        ncc_parse_tree_t *parent =
            (ncc_parse_tree_t *)ncc_tree_node_value(cur).parent;
        if (!parent) {
            return false;
        }
        if (ncc_xform_nt_name_is(parent, "struct_or_union_specifier")) {
            return is_n00b_variant_generic_struct(parent);
        }
        cur = parent;
    }
    return false;
}

static void
walk(ncc_parse_tree_t *n, bool error_on_union, int *count)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }

    if (ncc_xform_nt_name_is(n, "struct_or_union_specifier")
        && ncc_layout_struct_or_union_is_union(n)
        && !ncc_layout_node_starts_in_system_header(n)
        && !is_variant_value_union(n)
        && !ncc_xform_subtree_carries_n00b_named_attr(n, "raw_union")) {

        uint32_t    line = 0;
        uint32_t    col  = 0;
        const char *file = first_leaf_loc(n, &line, &col);
        fprintf(stderr,
                "%s:%u:%u: ncc: %s: traditional C union is discouraged; use "
                "n00b_variant_t for a GC-precise, marshalable tagged union. "
                "Raw unions may become an error in a future release (suppress "
                "with `union [[n00b::raw_union]] { ... }`)\n",
                file ? file : "<unknown>", line, col,
                error_on_union ? "error" : "warning");
        (*count)++;
    }

    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        walk(ncc_tree_child(n, i), error_on_union, count);
    }
}

int
ncc_union_deprecation_check(ncc_parse_tree_t *tu,
                            bool              allow_unions,
                            bool              error_on_union)
{
    if (tu == nullptr || allow_unions) {
        return 0;
    }
    int count = 0;
    walk(tu, error_on_union, &count);
    return count;
}
