// xform_generic_struct.c — Transform: _generic_struct deduplication.
//
// When ncc sees `_generic_struct tag { members };`:
//   - First time for this tag: emit full `struct tag { members };` just
//     before the enclosing external_declaration in translation_unit.
//   - Always: rewrite in-place to just `struct tag` (strip the body).
//
// A bare `_generic_struct tag` (no body) is always rewritten to `struct tag`.
//
// This allows multiple headers to contain the same _generic_struct definition
// without triggering duplicate struct errors — first occurrence wins.
//
// Insertion is before the enclosing declaration (not at position 0) so that
// types referenced in the struct body (e.g., typedefs from earlier headers)
// are already visible.

#include "lib/alloc.h"
#include "lib/dict.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Access to shared user_data dictionary
// ============================================================================

static ncc_dict_t *get_gs_decls(ncc_xform_ctx_t *ctx) {
  ncc_xform_data_t *d = ncc_xform_get_data(ctx);
  return &d->generic_struct_decls;
}

// ============================================================================
// Helpers
// ============================================================================

static ncc_parse_tree_t *parse_template(ncc_grammar_t *g, const char *nt_name,
                                        const char *src) {
  return ncc_xform_parse_source(g, nt_name, src, "xform_generic_struct");
}

// ============================================================================
// Transform: _generic_struct on struct_or_union_specifier (post-order)
// ============================================================================

static ncc_parse_tree_t *xform_generic_struct(ncc_xform_ctx_t *ctx,
                                              ncc_parse_tree_t *node) {
  size_t nc = ncc_tree_num_children(node);
  if (nc < 2) {
    return nullptr;
  }

  // Check child 0 is _kw_generic_struct NT.
  ncc_parse_tree_t *kw_nt = ncc_tree_child(node, 0);
  if (!kw_nt || !ncc_xform_nt_name_is(kw_nt, "_kw_generic_struct")) {
    return nullptr;
  }

  // Find the tag_name child (required).
  ncc_parse_tree_t *tag_node = ncc_xform_find_child_nt(node, "tag_name");
  if (!tag_node) {
    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);
    fprintf(stderr,
            "ncc: error: _generic_struct requires a tag name "
            "(line %u, col %u)\n",
            line, col);
    exit(1);
  }

  // Extract tag text.
  ncc_string_t tag_text = ncc_xform_node_to_text(tag_node);
  if (!tag_text.data || tag_text.data[0] == '\0') {
    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);
    fprintf(stderr,
            "ncc: error: _generic_struct has empty tag name "
            "(line %u, col %u)\n",
            line, col);
    exit(1);
  }

  // Trim whitespace from tag.
  char *p = tag_text.data;
  while (*p == ' ' || *p == '\t')
    p++;
  size_t len = strlen(p);
  while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
    len--;
  char tag[256];
  if (len >= sizeof(tag))
    len = sizeof(tag) - 1;
  memcpy(tag, p, len);
  tag[len] = '\0';
  ncc_free(tag_text.data);

  // Check if this is a definition (has body) or bare reference.
  bool has_body =
      (ncc_xform_find_child_nt(node, "member_declaration_list") != nullptr);

  // Also check empty body: _generic_struct tag { }
  // The grammar alternative with %"{" %"}" has at least 4 children:
  // _kw_generic_struct, tag_name, "{", "}"
  if (!has_body && nc >= 4) {
    // Check if there's a "{" leaf among children.
    for (size_t i = 0; i < nc; i++) {
      ncc_parse_tree_t *c = ncc_tree_child(node, i);
      if (c && ncc_tree_is_leaf(c)) {
        const char *txt = ncc_xform_leaf_text(c);
        if (txt && strcmp(txt, "{") == 0) {
          has_body = true;
          break;
        }
      }
    }
  }

  if (has_body) {
    ncc_dict_t *decls = get_gs_decls(ctx);

    if (!ncc_dict_contains(decls, (void *)tag)) {
      ncc_dict_put(decls, strdup(tag), (void *)(uintptr_t)1);

      // Serialize the full node, replacing _generic_struct with struct.
      ncc_string_t full_str = ncc_xform_node_to_text(node);
      if (!full_str.data) {
        fprintf(stderr,
                "ncc: error: failed to serialize _generic_struct '%s'\n", tag);
        exit(1);
      }

      // Replace "_generic_struct" prefix with "struct".
      // Work with a mutable copy (full_str.data is already heap-allocated).
      char *full_text = full_str.data;
      const char *prefix = "_generic_struct";
      size_t plen = strlen(prefix);
      char *pos = strstr(full_text, prefix);
      if (pos) {
        size_t flen = strlen(full_text);
        size_t before = (size_t)(pos - full_text);
        size_t after = flen - before - plen;
        size_t new_len = before + 6 + after + 1; // "struct" = 6
        char *repl = ncc_alloc_size(1, new_len);
        memcpy(repl, full_text, before);
        memcpy(repl + before, "struct", 6);
        memcpy(repl + before + 6, pos + plen, after);
        repl[new_len - 1] = '\0';
        ncc_free(full_text);
        full_text = repl;
      }

      // Ensure trailing semicolon.
      size_t flen = strlen(full_text);
      char *src;
      if (flen > 0 && full_text[flen - 1] != ';') {
        src = ncc_alloc_size(1, flen + 2);
        memcpy(src, full_text, flen);
        src[flen] = ';';
        src[flen + 1] = '\0';
        ncc_free(full_text);
      } else {
        src = full_text;
      }

      ncc_parse_tree_t *decl_tree =
          parse_template(ctx->grammar, "external_declaration", src);
      ncc_free(src);

      if (!decl_tree) {
        fprintf(stderr, "ncc: error: failed to emit generic struct '%s'\n",
                tag);
        exit(1);
      }

      // Add trailing newline trivia.
      ncc_token_info_t *last_tok = ncc_xform_find_last_leaf_token(decl_tree);
      if (last_tok) {
        ncc_trivia_t *nl = ncc_alloc(ncc_trivia_t);
        nl->text = ncc_string_from_cstr("\n");
        nl->next = last_tok->trailing_trivia;
        last_tok->trailing_trivia = nl;
      }

      // Find the enclosing external_declaration and insert before it.
      // This keeps the emitted struct after all preceding typedefs
      // and includes, so types in the struct body are visible.
      ncc_parse_tree_t *ext_decl =
          ncc_xform_find_ancestor(node, "external_declaration");
      size_t insert_pos = 0;

      // Walk up from ext_decl to find its direct parent container
      // (a $$group node or the translation_unit) and the position
      // of ext_decl within it.  Insert the struct definition
      // just before ext_decl so preceding typedefs are visible.
      {
        ncc_nt_node_t pn = ncc_tree_node_value(ext_decl);
        ncc_parse_tree_t *container = pn.parent;

        if (!container) {
          container = ctx->root;
        }

        size_t cnc = ncc_tree_num_children(container);
        for (size_t i = 0; i < cnc; i++) {
          if (ncc_tree_child(container, i) == ext_decl) {
            insert_pos = i;
            break;
          }
        }

        ncc_xform_insert_child(container, insert_pos, decl_tree);
      }
    }
  }

  // Always replace with just `struct tag`.
  char ref[280];
  snprintf(ref, sizeof(ref), "struct %s", tag);

  ncc_parse_tree_t *replacement =
      parse_template(ctx->grammar, "type_specifier", ref);
  if (!replacement) {
    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);
    fprintf(stderr,
            "ncc: error: failed to create 'struct %s' replacement "
            "(line %u, col %u)\n",
            tag, line, col);
    exit(1);
  }

  return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_generic_struct_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "struct_or_union_specifier", xform_generic_struct,
                     "generic_struct");
}
