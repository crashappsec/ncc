// xform_typehash.c — Transform: typehash(T) -> numeric literal.
//
// Registered as post-order on "primary_expression".
// typehash(int *) becomes 12345678901234ULL.

#include "xform/xform_helpers.h"
#include "xform/xform_gc_typemap.h"
#include "util/type_normalize.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Transform callback
// ============================================================================

static ncc_parse_tree_t *
xform_typehash(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    // primary_expression has many alternatives. The typehash one is:
    //   "typehash" "(" <typeid_atom> <typeid_continuation>? ")"
    //
    // Check that first child is the "typehash" keyword token.

    size_t nc = ncc_tree_num_children(node);
    if (nc < 4) {
        return nullptr;
    }

    ncc_parse_tree_t *first = ncc_tree_child(node, 0);
    if (!ncc_xform_leaf_text_eq(first, "typehash")) {
        return nullptr;
    }

    ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
    ncc_parse_tree_t *cont = ncc_xform_find_child_nt(node,
                                                        "typeid_continuation");
    if (!atom) {
        return nullptr;
    }

    // typehash(<expression>): hash the expression's inferred type (this is
    // typehash(typeof(expr)) without spelling out typeof). The type model
    // recomputes the canonical type spelling identically to a written type, so
    // the hash matches. Also catches expressions the grammar mis-parsed as
    // type_names (e.g. `lp[2]`).
    char *inferred = ncc_xform_expr_arg_type(ctx, atom, cont);
    if (inferred) {
        uint64_t hash = ncc_type_hash_u64(inferred);
        ncc_gc_typemap_note(ctx, inferred, hash);

        char buf[32];
        snprintf(buf, sizeof(buf), "%" PRIu64 "ULL", hash);
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        ncc_parse_tree_t *replacement = ncc_xform_make_token_node(
            NCC_TOK_INTEGER, buf, line, col);
        ncc_free(inferred);
        return replacement;
    }

    ncc_string_t type_str = ncc_xform_extract_type_string(ctx, atom, cont);
    uint64_t     hash     = ncc_type_hash_u64(type_str.data);

    // WP-020 / D-049: record pointer-to-aggregate typehashes so we can emit a
    // matching link-time GC pointer-map entry (same hash -> guaranteed match).
    ncc_gc_typemap_note(ctx, type_str.data, hash);

    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRIu64 "ULL", hash);

    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);

    ncc_parse_tree_t *replacement = ncc_xform_make_token_node(
        NCC_TOK_INTEGER, buf, line, col);

    ncc_free(type_str.data);
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_register_typehash_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "primary_expression", xform_typehash,
                         "typehash");
}
