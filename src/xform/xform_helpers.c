// xform_helpers.c — Shared utilities for type-introspection transforms.

#include "xform/xform_helpers.h"
#include "xform/xform_template.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Leaf text access
// ============================================================================

const char *ncc_xform_leaf_text(ncc_parse_tree_t *node) {
  if (!node || !ncc_tree_is_leaf(node)) {
    return nullptr;
  }

  ncc_token_info_t *tok = ncc_tree_leaf_value(node);
  if (!tok || !ncc_option_is_set(tok->value)) {
    return nullptr;
  }

  ncc_string_t s = ncc_option_get(tok->value);
  return s.data;
}

bool ncc_xform_leaf_text_eq(ncc_parse_tree_t *node, const char *text) {
  const char *leaf = ncc_xform_leaf_text(node);
  if (!leaf || !text) {
    return false;
  }
  return strcmp(leaf, text) == 0;
}

// ============================================================================
// First leaf position
// ============================================================================

void ncc_xform_first_leaf_pos(ncc_parse_tree_t *node, uint32_t *line,
                              uint32_t *col) {
  *line = 0;
  *col = 0;

  if (!node) {
    return;
  }

  if (ncc_tree_is_leaf(node)) {
    ncc_token_info_t *tok = ncc_tree_leaf_value(node);
    if (tok) {
      *line = tok->line;
      *col = tok->column;
    }
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_xform_first_leaf_pos(ncc_tree_child(node, i), line, col);
    if (*line != 0) {
      return;
    }
  }
}


// ============================================================================
// NT name check and child search
// ============================================================================

bool ncc_xform_nt_name_is(ncc_parse_tree_t *node, const char *name) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  if (!pn.name.data) {
    return false;
  }
  return strcmp(pn.name.data, name) == 0;
}

ncc_parse_tree_t *ncc_xform_find_child_nt(ncc_parse_tree_t *node,
                                          const char *name) {
  if (!node || ncc_tree_is_leaf(node)) {
    return nullptr;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(node, i);
    if (!c || ncc_tree_is_leaf(c)) {
      continue;
    }
    if (ncc_xform_nt_name_is(c, name)) {
      return c;
    }
    // Unwrap group wrapper nodes ($$group_*).
    ncc_nt_node_t pn = ncc_tree_node_value(c);
    if (pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$') {
      ncc_parse_tree_t *inner = ncc_xform_find_child_nt(c, name);
      if (inner) {
        return inner;
      }
    }
  }
  return nullptr;
}

// ============================================================================
// Shared parse helper
// ============================================================================

ncc_parse_tree_t *ncc_xform_parse_source(ncc_grammar_t *g, const char *nt_name,
                                          const char *src,
                                          const char *xform_name) {
  ncc_result_t(ncc_parse_tree_ptr_t) r =
      ncc_xform_parse_template(g, nt_name, src, nullptr);
  if (ncc_result_is_err(r)) {
    fprintf(stderr, "%s: template parse failed for '%s':\n  %s\n",
            xform_name ? xform_name : "xform", nt_name, src);
    return nullptr;
  }
  return ncc_result_get(r);
}

// ============================================================================
// Last leaf token
// ============================================================================

ncc_token_info_t *ncc_xform_find_last_leaf_token(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_tree_leaf_value(node);
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = nc; i > 0; i--) {
    ncc_token_info_t *tok =
        ncc_xform_find_last_leaf_token(ncc_tree_child(node, i - 1));
    if (tok) {
      return tok;
    }
  }
  return nullptr;
}

// ============================================================================
// First leaf text
// ============================================================================

const char *ncc_xform_get_first_leaf_text(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text(node);
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    const char *t = ncc_xform_get_first_leaf_text(ncc_tree_child(node, i));
    if (t) {
      return t;
    }
  }
  return nullptr;
}

// ============================================================================
// Void type check
// ============================================================================

bool ncc_xform_has_void_type(ncc_parse_tree_t *node) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, "void");
  }
  if (ncc_xform_nt_name_is(node, "storage_class_specifier") ||
      ncc_xform_nt_name_is(node, "function_specifier")) {
    return false;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (ncc_xform_has_void_type(ncc_tree_child(node, i))) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// Collect base type from declaration_specifiers
// ============================================================================

static void collect_nt_text(ncc_parse_tree_t *node, const char *nt_name,
                            ncc_buffer_t *buf) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  if (ncc_xform_nt_name_is(node, nt_name)) {
    ncc_string_t text = ncc_xform_node_to_text(node);
    if (text.data) {
      if (buf->byte_len > 0) {
        ncc_buffer_putc(buf, ' ');
      }
      ncc_buffer_append(buf, text.data, text.u8_bytes);
      ncc_free(text.data);
    }
    return;
  }

  if (ncc_xform_nt_name_is(node, "attribute_specifier_sequence")) {
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_nt_text(ncc_tree_child(node, i), nt_name, buf);
  }
}

char *ncc_xform_collect_base_type(ncc_parse_tree_t *decl_specs) {
  ncc_buffer_t *buf = ncc_buffer_empty();

  collect_nt_text(decl_specs, "type_specifier_qualifier", buf);

  char *result = ncc_buffer_take(buf);
  if (!result || result[0] == '\0') {
    ncc_free(result);
    result = ncc_alloc_size(1, 4);
    memcpy(result, "int", 4);
  }
  return result;
}

// ============================================================================
// Collect qualifiers from declaration_specifiers
// ============================================================================

// Helper: check if a subtree contains a leaf matching `text`.
static bool contains_leaf_text(ncc_parse_tree_t *node, const char *text) {
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

char *ncc_xform_collect_qualifiers(ncc_parse_tree_t *decl_specs,
                                    const char *skip) {
  ncc_buffer_t *buf = ncc_buffer_empty();

  struct { ncc_parse_tree_t *node; } stack[128];
  int sp = 0;
  stack[sp++].node = decl_specs;

  while (sp > 0) {
    ncc_parse_tree_t *node = stack[--sp].node;
    if (!node || ncc_tree_is_leaf(node)) {
      continue;
    }

    if (ncc_xform_nt_name_is(node, "storage_class_specifier") ||
        ncc_xform_nt_name_is(node, "function_specifier")) {
      if (skip && contains_leaf_text(node, skip)) {
        continue;
      }
      ncc_string_t text = ncc_xform_node_to_text(node);
      if (text.data) {
        if (buf->byte_len > 0) {
          ncc_buffer_putc(buf, ' ');
        }
        ncc_buffer_append(buf, text.data, text.u8_bytes);
        ncc_free(text.data);
      }
      continue;
    }

    if (ncc_xform_nt_name_is(node, "attribute_specifier_sequence")) {
      continue;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = nc; i > 0; i--) {
      if (sp < 128) {
        stack[sp++].node = ncc_tree_child(node, i - 1);
      }
    }
  }

  return ncc_buffer_take(buf);
}

// ============================================================================
// Type atom processing
// ============================================================================

// Check whether a non-leaf node is a PWZ group wrapper ($$group_*).
static bool is_group_node(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$';
}

// Recursively collect string token leaves from a string_literal subtree,
// stripping quotes.  Handles group wrapper nodes transparently.
static void collect_string_leaves(ncc_parse_tree_t *node, ncc_buffer_t *cb) {
  if (!node) {
    return;
  }

  if (ncc_tree_is_leaf(node)) {
    const char *text = ncc_xform_leaf_text(node);
    if (!text) {
      return;
    }
    size_t tlen = strlen(text);
    if (tlen >= 2 && text[0] == '"' && text[tlen - 1] == '"') {
      size_t inner_len = tlen - 2;
      char *inner = ncc_alloc_size(1, inner_len + 1);
      memcpy(inner, text + 1, inner_len);
      inner[inner_len] = '\0';
      ncc_buffer_puts(cb, inner);
      ncc_free(inner);
    } else {
      ncc_buffer_puts(cb, text);
    }
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_string_leaves(ncc_tree_child(node, i), cb);
  }
}

// Extract the text content from a string_literal subtree, stripping quotes.
static char *extract_string_literal_text(ncc_parse_tree_t *node) {
  ncc_buffer_t *cb = ncc_buffer_empty();
  collect_string_leaves(node, cb);
  char *result = ncc_buffer_take(cb);
  return result;
}

// Process a single <typeid_atom>.
static void process_atom(ncc_parse_tree_t *atom, ncc_buffer_t *out) {
  if (!atom) {
    return;
  }

  // Unwrap group wrappers around the atom itself.
  while (is_group_node(atom) && ncc_tree_num_children(atom) > 0) {
    atom = ncc_tree_child(atom, 0);
  }

  // <typeid_atom> ::= <string_literal> | <typeof_specifier_argument>
  // The atom node itself is the NT; look at child(0).
  // Try to find the actual content child, unwrapping groups.
  ncc_parse_tree_t *child0 = nullptr;

  size_t nc = ncc_tree_num_children(atom);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(atom, i);
    if (!c) {
      continue;
    }
    if (ncc_tree_is_leaf(c)) {
      continue; // Skip punctuation tokens.
    }
    if (is_group_node(c)) {
      // Look inside the group for a real NT.
      ncc_parse_tree_t *inner = ncc_xform_find_child_nt(c, "string_literal");
      if (inner) {
        child0 = inner;
        break;
      }
      inner = ncc_xform_find_child_nt(c, "typeof_specifier_argument");
      if (inner) {
        child0 = inner;
        break;
      }
      continue;
    }
    child0 = c;
    break;
  }

  if (!child0) {
    return;
  }

  if (ncc_xform_nt_name_is(child0, "string_literal")) {
    char *text = extract_string_literal_text(child0);
    ncc_buffer_puts(out, text);
    ncc_free(text);
  } else {
    // typeof_specifier_argument or direct child — normalize the subtree.
    ncc_string_t normalized = ncc_normalize_type_tree(child0);
    if (normalized.data) {
      ncc_buffer_puts(out, normalized.data);
      ncc_free(normalized.data);
    }
  }
}

// Process <typeid_continuation> recursively.
static void process_continuation(ncc_parse_tree_t *cont, ncc_buffer_t *out) {
  if (!cont) {
    return;
  }

  // Unwrap group wrappers to find the actual typeid_continuation.
  while (is_group_node(cont)) {
    ncc_parse_tree_t *inner =
        ncc_xform_find_child_nt(cont, "typeid_continuation");
    if (inner) {
      cont = inner;
      break;
    }
    // If no typeid_continuation inside group, try first child.
    if (ncc_tree_num_children(cont) > 0) {
      cont = ncc_tree_child(cont, 0);
    } else {
      return;
    }
  }

  if (!ncc_xform_nt_name_is(cont, "typeid_continuation")) {
    return;
  }

  // <typeid_continuation> ::= "," <typeid_atom> <typeid_continuation>?
  //                         | ","
  // Find the typeid_atom child (may be wrapped in group).
  ncc_parse_tree_t *atom = ncc_xform_find_child_nt(cont, "typeid_atom");
  if (atom) {
    process_atom(atom, out);
  }

  // Find optional nested typeid_continuation.
  ncc_parse_tree_t *next = ncc_xform_find_child_nt(cont, "typeid_continuation");
  if (next && next != cont) {
    process_continuation(next, out);
  } else {
    // Also check if there's a group child containing continuation.
    size_t nc = ncc_tree_num_children(cont);
    for (size_t i = 0; i < nc; i++) {
      ncc_parse_tree_t *c = ncc_tree_child(cont, i);
      if (is_group_node(c)) {
        ncc_parse_tree_t *inner =
            ncc_xform_find_child_nt(c, "typeid_continuation");
        if (inner) {
          process_continuation(inner, out);
          break;
        }
      }
    }
  }
}

// ============================================================================
// Node-to-text: concatenate all leaf text in a subtree
// ============================================================================

static void collect_leaf_text(ncc_parse_tree_t *node, ncc_buffer_t *cb) {
  if (!node) {
    return;
  }
  if (ncc_tree_is_leaf(node)) {
    const char *text = ncc_xform_leaf_text(node);
    if (text) {
      if (cb->byte_len > 0) {
        ncc_buffer_puts(cb, " ");
      }
      ncc_buffer_puts(cb, text);
    }
    return;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_leaf_text(ncc_tree_child(node, i), cb);
  }
}

ncc_string_t ncc_xform_node_to_text(ncc_parse_tree_t *node) {
  ncc_buffer_t *cb = ncc_buffer_empty();
  collect_leaf_text(node, cb);
  ncc_string_t result = ncc_string_from_raw(cb->data, (int64_t)cb->byte_len);
  ncc_free(cb->data);
  ncc_free(cb);
  return result;
}

// ============================================================================
// Type string extraction
// ============================================================================

ncc_string_t ncc_xform_extract_type_string(ncc_xform_ctx_t *ctx,
                                           ncc_parse_tree_t *atom_node,
                                           ncc_parse_tree_t *cont_node) {
  (void)ctx;

  ncc_buffer_t *cb = ncc_buffer_empty();

  process_atom(atom_node, cb);
  process_continuation(cont_node, cb);

  ncc_string_t result = ncc_string_from_raw(cb->data, (int64_t)cb->byte_len);
  ncc_free(cb->data);
  ncc_free(cb);
  return result;
}
