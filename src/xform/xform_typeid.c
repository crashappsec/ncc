// xform_typeid.c — Transform: typeid(T) -> mangled identifier.
//
// Registered as post-order on "synthetic_identifier".
// typeid(int *) becomes __aB3cD5eF... (SHA256-based identifier).

#include "xform/xform_helpers.h"

#include <stdlib.h>

// ============================================================================
// Transform callback
// ============================================================================

static ncc_parse_tree_t *
xform_typeid(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    // synthetic_identifier alternatives:
    //   1: <_named_id_kw_typeid> "(" <typeid_atom> <typeid_continuation>? ")"
    //   2: "constexpr_paste" "(" <argument_expression_list> ")"
    //
    // For alt 1, child[0] is the <_named_id_kw_typeid> NT.
    // We need to find the "typeid" keyword leaf inside it.

    size_t nc = ncc_tree_num_children(node);
    if (nc < 4) {
        return nullptr;
    }

    // Check that first child is the _named_id_kw_typeid NT containing "typeid".
    ncc_parse_tree_t *kw_nt = ncc_tree_child(node, 0);
    if (!kw_nt) {
        return nullptr;
    }

    // The kw NT has one leaf child with the keyword text.
    const char *kw_text = nullptr;
    if (ncc_tree_is_leaf(kw_nt)) {
        kw_text = ncc_xform_leaf_text(kw_nt);
    }
    else if (ncc_tree_num_children(kw_nt) > 0) {
        kw_text = ncc_xform_leaf_text(ncc_tree_child(kw_nt, 0));
    }

    if (!kw_text || strcmp(kw_text, "typeid") != 0) {
        return nullptr;
    }

    // Find <typeid_atom> and optional <typeid_continuation> by name,
    // since group wrapper nodes may alter child indices.
    ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
    ncc_parse_tree_t *cont = ncc_xform_find_child_nt(node,
                                                        "typeid_continuation");

    if (!atom) {
        return nullptr;
    }

    ncc_string_t type_str = ncc_xform_extract_type_string(ctx, atom, cont);
    ncc_string_t mangled  = ncc_type_mangle(type_str.data);

    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);

    ncc_parse_tree_t *replacement = ncc_xform_make_token_node(
        NCC_TOK_IDENTIFIER, mangled.data, line, col);

    ncc_free(type_str.data);
    ncc_free(mangled.data);
    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_register_typeid_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "synthetic_identifier", xform_typeid,
                         "typeid");
}
