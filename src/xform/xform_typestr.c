// xform_typestr.c — Transform: typestr(T) -> string literal.
//
// Registered as post-order on "synthetic_string_literal".
// typestr(int *) becomes "int *".

#include "lib/alloc.h"
#include "xform/xform_helpers.h"

#include <stdlib.h>
#include <string.h>

// ============================================================================
// Transform callback
// ============================================================================

static ncc_parse_tree_t *xform_typestr(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *node) {
  // synthetic_string_literal alternatives:
  //   1: <_named_id_kw_typestr> "(" <typeid_atom> <typeid_continuation>? ")"
  //   2: <string_literal>
  //
  // For alt 2, we pass through (return nullptr).
  // For alt 1, we transform.

  size_t nc = ncc_tree_num_children(node);
  if (nc < 4) {
    return nullptr; // Probably alt 2 (string_literal).
  }

  // Check that first child contains the "typestr" keyword.
  ncc_parse_tree_t *kw_nt = ncc_tree_child(node, 0);
  if (!kw_nt) {
    return nullptr;
  }

  const char *kw_text = nullptr;
  if (ncc_tree_is_leaf(kw_nt)) {
    kw_text = ncc_xform_leaf_text(kw_nt);
  } else if (ncc_tree_num_children(kw_nt) > 0) {
    kw_text = ncc_xform_leaf_text(ncc_tree_child(kw_nt, 0));
  }

  if (!kw_text || strcmp(kw_text, "typestr") != 0) {
    return nullptr;
  }

  ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
  ncc_parse_tree_t *cont = ncc_xform_find_child_nt(node, "typeid_continuation");
  if (!atom) {
    return nullptr;
  }

  ncc_string_t type_str = ncc_xform_extract_type_string(ctx, atom, cont);

  // Build quoted string: "\"type_str\""
  size_t len = type_str.u8_bytes;
  char *quoted = ncc_alloc_size(1, len + 3);
  quoted[0] = '"';
  memcpy(quoted + 1, type_str.data, len);
  quoted[len + 1] = '"';
  quoted[len + 2] = '\0';

  uint32_t line, col;
  ncc_xform_first_leaf_pos(node, &line, &col);

  ncc_parse_tree_t *replacement =
      ncc_xform_make_token_node(NCC_TOK_STRING_LIT, quoted, line, col);

  ncc_free(type_str.data);
  ncc_free(quoted);
  return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_typestr_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "synthetic_string_literal", xform_typestr, "typestr");
}
