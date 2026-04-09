// xform_kargs_vargs.c — Unified keyword arguments (_kargs) and variadic
// arguments (+) transform.
//
// Registered callbacks:
//   - Pre-order on "function_definition": extract metadata, emit struct,
//     rewrite signature, inject body extraction code.
//   - Pre-order on "declaration": extract metadata from forward declarations
//     with _kargs, rewrite signature.
//   - Post-order on "postfix_expression": transform call sites.

#include "lib/alloc.h"
#include "lib/string.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Accessor for ncc_xform_data_t.vargs_type
// ============================================================================

static const char *get_vargs_type(ncc_xform_ctx_t *ctx) {
  ncc_xform_data_t *d = ncc_xform_get_data(ctx);
  return d->vargs_type;
}

// ============================================================================
// Metadata types
// ============================================================================

typedef struct {
  ncc_string_t name;         // param name
  ncc_string_t type_text;    // full type as text (e.g. "int", "const char *")
  ncc_string_t default_text; // default as text, empty = no default
  ncc_parse_tree_t *decl_specs;  // cloned declaration_specifiers subtree
  ncc_parse_tree_t *declarator;  // cloned declarator subtree
  ncc_parse_tree_t *default_val; // cloned initializer subtree (nullptr = none)
} ncc_kw_param_t;

typedef struct {
  ncc_kw_param_t *params;
  int num_params;
  int num_positional;
  bool defaults_set;
  bool struct_emitted;
  bool is_opaque;
} ncc_kw_info_t;

typedef enum {
  NCC_VARGS_NONE,
  NCC_VARGS_N00B,
  NCC_VARGS_C,
} ncc_vargs_style_t;

typedef struct {
  ncc_vargs_style_t style;
  int num_positional;
  ncc_parse_tree_t *type_node;
} ncc_vargs_info_t;

typedef struct {
  ncc_kw_info_t *kw;
  ncc_vargs_info_t *va;
} ncc_func_meta_t;

// Simple hash table for function metadata.
#define META_TABLE_SIZE 256

typedef struct {
  char *key;
  ncc_func_meta_t *value;
} meta_entry_t;

typedef struct {
  meta_entry_t entries[META_TABLE_SIZE];
} meta_table_t;

// Access the meta_table from ctx->user_data.
static meta_table_t *get_meta_table(ncc_xform_ctx_t *ctx) {
  ncc_xform_data_t *d = ncc_xform_get_data(ctx);
  // meta_table_t and ncc_meta_table_t have identical layout.
  return (meta_table_t *)&d->func_meta;
}

// ============================================================================
// Hash table operations
// ============================================================================

static uint32_t hash_str(const char *s) {
  uint32_t h = 5381;
  for (; *s; s++) {
    h = ((h << 5) + h) + (unsigned char)*s;
  }
  return h;
}

static ncc_func_meta_t *meta_lookup(meta_table_t *table, const char *name) {
  uint32_t idx = hash_str(name) % META_TABLE_SIZE;
  for (uint32_t i = 0; i < META_TABLE_SIZE; i++) {
    uint32_t slot = (idx + i) % META_TABLE_SIZE;
    if (!table->entries[slot].key) {
      return nullptr;
    }
    if (strcmp(table->entries[slot].key, name) == 0) {
      return table->entries[slot].value;
    }
  }
  return nullptr;
}

static ncc_func_meta_t *meta_insert(meta_table_t *table, const char *name) {
  uint32_t idx = hash_str(name) % META_TABLE_SIZE;
  for (uint32_t i = 0; i < META_TABLE_SIZE; i++) {
    uint32_t slot = (idx + i) % META_TABLE_SIZE;
    if (!table->entries[slot].key) {
      table->entries[slot].key = strdup(name);
      table->entries[slot].value = ncc_alloc(ncc_func_meta_t);
      return table->entries[slot].value;
    }
    if (strcmp(table->entries[slot].key, name) == 0) {
      return table->entries[slot].value;
    }
  }
  fprintf(stderr, "ncc: error: function metadata table full\n");
  exit(1);
}

// ============================================================================
// Resolve embedded typeid(...) / typehash(...) in text strings.
//
// Scans for occurrences of "typeid" or "typehash" followed by balanced
// parentheses and replaces them with their resolved values.  Handles
// nesting (e.g. typehash(struct typeid("a", int))).
// ============================================================================

static ncc_string_t resolve_ncc_type_calls(const char *text) {
  if (!text) {
    return (ncc_string_t){0};
  }

  // Quick check — if no "typeid" or "typehash" in text, return copy.
  if (!strstr(text, "typeid") && !strstr(text, "typehash")) {
    return ncc_string_from_cstr(text);
  }

  char result[16384];
  size_t rpos = 0;
  const char *p = text;

  while (*p) {
    // Look for "typeid" or "typehash" token boundaries.
    bool is_typeid =
        (strncmp(p, "typeid", 6) == 0 &&
         !((p[6] >= 'a' && p[6] <= 'z') || (p[6] >= 'A' && p[6] <= 'Z') ||
           (p[6] >= '0' && p[6] <= '9') || p[6] == '_'));
    bool is_typehash =
        (strncmp(p, "typehash", 8) == 0 &&
         !((p[8] >= 'a' && p[8] <= 'z') || (p[8] >= 'A' && p[8] <= 'Z') ||
           (p[8] >= '0' && p[8] <= '9') || p[8] == '_'));

    if (!is_typeid && !is_typehash) {
      result[rpos++] = *p++;
      continue;
    }

    size_t kw_len = is_typeid ? 6 : 8;
    const char *after_kw = p + kw_len;

    // Skip whitespace after keyword.
    while (*after_kw == ' ') {
      after_kw++;
    }

    if (*after_kw != '(') {
      // Not a call — copy the keyword literally.
      for (size_t i = 0; i < kw_len; i++) {
        result[rpos++] = p[i];
      }
      p += kw_len;
      continue;
    }

    // Find matching close paren.
    const char *start = after_kw; // points to '('
    int depth = 0;
    const char *q = start;
    while (*q) {
      if (*q == '(') {
        depth++;
      } else if (*q == ')') {
        depth--;
        if (depth == 0) {
          break;
        }
      }
      q++;
    }

    if (depth != 0) {
      // Unbalanced — copy literally.
      result[rpos++] = *p++;
      continue;
    }

    // Extract inner content between parens.
    size_t inner_len = (size_t)(q - start - 1);
    char *inner = ncc_alloc_size(1, inner_len + 1);
    memcpy(inner, start + 1, inner_len);
    inner[inner_len] = '\0';

    // Recursively resolve nested calls in the inner content.
    ncc_string_t resolved_inner = resolve_ncc_type_calls(inner);
    ncc_free(inner);

    if (is_typeid) {
      // Build the type string from the comma-separated arguments.
      // Each argument may be a quoted string or a type expression.
      // Strip quotes from string arguments, concatenate all.
      char type_str[4096];
      size_t tpos = 0;
      const char *r = resolved_inner.data;

      while (*r) {
        // Skip leading whitespace.
        while (*r == ' ') {
          r++;
        }
        if (*r == ',') {
          r++;
          continue;
        }
        if (*r == '"') {
          // Quoted string argument — extract content.
          r++;
          while (*r && *r != '"') {
            type_str[tpos++] = *r++;
          }
          if (*r == '"') {
            r++;
          }
        } else {
          // Type expression — copy until comma or end.
          while (*r && *r != ',') {
            type_str[tpos++] = *r++;
          }
          // Trim trailing whitespace from type expression.
          while (tpos > 0 && type_str[tpos - 1] == ' ') {
            tpos--;
          }
        }
      }
      type_str[tpos] = '\0';

      ncc_string_t mangled = ncc_type_mangle(type_str);
      memcpy(result + rpos, mangled.data, mangled.u8_bytes);
      rpos += mangled.u8_bytes;
      ncc_free(mangled.data);
    } else {
      // typehash — compute the uint64 hash.
      // The inner content is a type expression (possibly with struct prefix).
      // Strip "struct" prefix if present for the hash computation.
      const char *type_for_hash = resolved_inner.data;
      while (*type_for_hash == ' ') {
        type_for_hash++;
      }

      // For typehash(struct __xxx), we need to resolve the actual type.
      // But the canonical path is: typehash uses the same type string
      // as typeid (before mangling). We need the original type args.
      // Since this is uncommon in kargs type_text, emit a warning.
      uint64_t hash = ncc_type_hash_u64(type_for_hash);
      char hash_buf[32];
      snprintf(hash_buf, sizeof(hash_buf), "%" PRIu64 "ULL", hash);
      size_t hlen = strlen(hash_buf);
      memcpy(result + rpos, hash_buf, hlen);
      rpos += hlen;
    }

    ncc_free(resolved_inner.data);
    p = q + 1; // skip past closing paren
  }

  result[rpos] = '\0';
  return ncc_string_from_cstr(result);
}

// ============================================================================
// Template parsing helper
// ============================================================================

static ncc_parse_tree_t *parse_template(ncc_grammar_t *g, const char *nt_name,
                                        const char *src) {
  return ncc_xform_parse_source(g, nt_name, src, "xform_kargs_vargs");
}

// ============================================================================
// Helpers: extract function name from declarator
// ============================================================================

static const char *extract_func_name(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    ncc_token_info_t *tok = ncc_tree_leaf_value(node);
    if (tok && tok->tid == (int32_t)NCC_TOK_IDENTIFIER) {
      return ncc_xform_leaf_text(node);
    }
    return nullptr;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    const char *name = extract_func_name(ncc_tree_child(node, i));
    if (name) {
      return name;
    }
  }
  return nullptr;
}

// Recursively find a synthetic_identifier node in the tree.
static ncc_parse_tree_t *find_synthetic_identifier(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return nullptr;
  }
  if (ncc_xform_nt_name_is(node, "synthetic_identifier")) {
    return node;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *r = find_synthetic_identifier(ncc_tree_child(node, i));
    if (r) {
      return r;
    }
  }
  return nullptr;
}

// Extract function name, resolving typeid() if the name is a
// synthetic_identifier.  Returns an ncc_string_t; caller must free .data.
static ncc_string_t extract_resolved_func_name(ncc_parse_tree_t *declarator) {
  if (!declarator) {
    return (ncc_string_t){0};
  }

  // Search for synthetic_identifier anywhere in the declarator tree.
  ncc_parse_tree_t *si = find_synthetic_identifier(declarator);
  if (si) {
    ncc_string_t text = ncc_xform_node_to_text(si);
    ncc_string_t resolved = resolve_ncc_type_calls(text.data);
    ncc_free(text.data);
    return resolved;
  }

  // Fall back to simple identifier extraction.
  const char *name = extract_func_name(declarator);
  return name ? ncc_string_from_cstr(name) : (ncc_string_t){0};
}

// ============================================================================
// Helpers: find child by NT name (all children, not just first)
// ============================================================================

static int find_child_index_nt(ncc_parse_tree_t *parent, const char *nt_name) {
  size_t nc = ncc_tree_num_children(parent);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(parent, i);
    if (c && !ncc_tree_is_leaf(c) && ncc_xform_nt_name_is(c, nt_name)) {
      return (int)i;
    }
  }
  return -1;
}

static int find_child_index(ncc_parse_tree_t *parent, ncc_parse_tree_t *child) {
  size_t nc = ncc_tree_num_children(parent);
  for (size_t i = 0; i < nc; i++) {
    if (ncc_tree_child(parent, i) == child) {
      return (int)i;
    }
  }
  return -1;
}

// Find index of direct child of parent that IS or CONTAINS (via group
// unwrapping) a node with the given NT name.
static int find_child_index_containing_nt(ncc_parse_tree_t *parent,
                                          const char *nt_name) {
  size_t nc = ncc_tree_num_children(parent);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(parent, i);
    if (!c || ncc_tree_is_leaf(c)) {
      continue;
    }
    if (ncc_xform_nt_name_is(c, nt_name)) {
      return (int)i;
    }
    // Check if this is a group wrapper containing the NT.
    ncc_nt_node_t pn = ncc_tree_node_value(c);
    if (pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$') {
      if (ncc_xform_find_child_nt(c, nt_name)) {
        return (int)i;
      }
    }
  }
  return -1;
}

// ============================================================================
// Helpers: find a leaf with specific text in a subtree
// ============================================================================

static ncc_parse_tree_t *find_leaf_with_text(ncc_parse_tree_t *node,
                                             const char *text) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, text) ? node : nullptr;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *r = find_leaf_with_text(ncc_tree_child(node, i), text);
    if (r) {
      return r;
    }
  }
  return nullptr;
}

// ============================================================================
// Helpers: check if parameter_type_list has "+" (n00b vargs)
// ============================================================================

static bool param_list_has_plus(ncc_parse_tree_t *declarator) {
  // Search declarator for function_declarator -> parameter_type_list
  // and look for a "+" leaf.
  ncc_parse_tree_t *fd =
      ncc_xform_find_child_nt(declarator, "function_declarator");
  if (!fd) {
    // Might be deeper (pointer -> direct_declarator -> function_declarator)
    ncc_parse_tree_t *dd =
        ncc_xform_find_child_nt(declarator, "direct_declarator");
    if (dd) {
      fd = ncc_xform_find_child_nt(dd, "function_declarator");
    }
  }
  if (!fd) {
    return false;
  }

  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(fd, "parameter_type_list");
  if (!ptl) {
    return false;
  }

  return find_leaf_with_text(ptl, "+") != nullptr;
}

// ============================================================================
// Helpers: count positional params in parameter_list
// ============================================================================

static int count_positional_params(ncc_parse_tree_t *param_list) {
  if (!param_list) {
    return 0;
  }
  // parameter_list ::= parameter_declaration ("," parameter_declaration)*
  // With group wrappers, just count parameter_declaration children.
  int count = 0;

  if (ncc_tree_is_leaf(param_list)) {
    return 0;
  }

  size_t nc = ncc_tree_num_children(param_list);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(param_list, i);
    if (!c) {
      continue;
    }
    if (ncc_xform_nt_name_is(c, "parameter_declaration")) {
      count++;
    } else if (!ncc_tree_is_leaf(c)) {
      // Group wrapper — recurse.
      count += count_positional_params(c);
    }
  }
  return count;
}

// ============================================================================
// Helpers: find function_declarator and parameter_type_list
// ============================================================================

static ncc_parse_tree_t *find_func_declarator(ncc_parse_tree_t *declarator) {
  if (!declarator) {
    return nullptr;
  }
  if (ncc_xform_nt_name_is(declarator, "function_declarator")) {
    return declarator;
  }
  if (ncc_tree_is_leaf(declarator)) {
    return nullptr;
  }
  size_t nc = ncc_tree_num_children(declarator);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *r = find_func_declarator(ncc_tree_child(declarator, i));
    if (r) {
      return r;
    }
  }
  return nullptr;
}

static ncc_parse_tree_t *find_param_type_list(ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *fd = find_func_declarator(declarator);
  if (!fd) {
    return nullptr;
  }
  return ncc_xform_find_child_nt(fd, "parameter_type_list");
}

// ============================================================================
// Extract keyword params from keyword_clause
// ============================================================================

static void collect_kw_params_from_list(ncc_parse_tree_t *node,
                                        ncc_kw_param_t **out_params,
                                        int *out_count, int *out_cap) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  if (ncc_xform_nt_name_is(node, "keyword_param")) {
    // keyword_param ::= declaration_specifiers declarator "=" initializer ";"
    //                 | declaration_specifiers declarator ";"
    ncc_parse_tree_t *ds =
        ncc_xform_find_child_nt(node, "declaration_specifiers");
    ncc_parse_tree_t *decl = ncc_xform_find_child_nt(node, "declarator");
    ncc_parse_tree_t *init = ncc_xform_find_child_nt(node, "initializer");

    if (!ds || !decl) {
      return;
    }

    if (*out_count >= *out_cap) {
      *out_cap = *out_cap ? *out_cap * 2 : 8;
      *out_params =
          ncc_realloc(*out_params, sizeof(ncc_kw_param_t) * (size_t)*out_cap);
    }

    ncc_kw_param_t *p = &(*out_params)[*out_count];
    memset(p, 0, sizeof(*p));

    const char *pname = extract_func_name(decl);
    p->name = ncc_string_from_cstr(pname ? pname : "");
    p->decl_specs = ncc_xform_clone(ds);
    p->declarator = ncc_xform_clone(decl);

    // Build full type text: declaration_specifiers + pointer from declarator.
    // declarator ::= pointer? direct_declarator
    // We need the pointer part (e.g. "*") if present.
    ncc_string_t ds_text = ncc_xform_node_to_text(ds);
    ncc_parse_tree_t *ptr = ncc_xform_find_child_nt(decl, "pointer");
    char *raw_type;
    if (ptr) {
      ncc_string_t ptr_text = ncc_xform_node_to_text(ptr);
      size_t len = ds_text.u8_bytes + ptr_text.u8_bytes + 2;
      raw_type = ncc_alloc_size(1, len);
      snprintf(raw_type, len, "%s %s", ds_text.data, ptr_text.data);
      ncc_free(ptr_text.data);
    } else {
      raw_type = strdup(ds_text.data);
    }
    ncc_free(ds_text.data);
    // Resolve any embedded typeid(...)/typehash(...) calls.
    p->type_text = resolve_ncc_type_calls(raw_type);
    ncc_free(raw_type);

    if (init) {
      p->default_val = ncc_xform_clone(init);
      ncc_string_t raw_default = ncc_xform_node_to_text(init);
      p->default_text = resolve_ncc_type_calls(raw_default.data);
      ncc_free(raw_default.data);
    }

    (*out_count)++;
    return;
  }

  // Recurse into group wrappers, keyword_param_list, etc.
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_kw_params_from_list(ncc_tree_child(node, i), out_params, out_count,
                                out_cap);
  }
}

// ============================================================================
// Extract vargs info from parameter_type_list
// ============================================================================

static ncc_vargs_info_t *extract_vargs_info(ncc_parse_tree_t *ptl) {
  if (!ptl) {
    return nullptr;
  }
  if (!find_leaf_with_text(ptl, "+")) {
    return nullptr;
  }

  ncc_vargs_info_t *vi = ncc_alloc(ncc_vargs_info_t);
  vi->style = NCC_VARGS_N00B;

  // Count positional params: those in parameter_list before the "+"
  ncc_parse_tree_t *pl = ncc_xform_find_child_nt(ptl, "parameter_list");
  vi->num_positional = count_positional_params(pl);

  // Check for typed vargs: <type_name> "+"
  ncc_parse_tree_t *tn = ncc_xform_find_child_nt(ptl, "type_name");
  if (tn) {
    vi->type_node = ncc_xform_clone(tn);
  }

  return vi;
}

// ============================================================================
// Generate kargs struct as external_declaration
// ============================================================================

static ncc_parse_tree_t *generate_kargs_struct(ncc_grammar_t *g,
                                               const char *func_name,
                                               ncc_kw_info_t *kw) {
  // Build source for struct:
  // struct _funcname__kargs {
  //     type1 param1;
  //     type2 param2;
  //     unsigned _has_param1:1;
  //     unsigned _has_param2:1;
  // };
  char buf[8192];
  size_t pos = 0;

  // Resolve any typeid/typehash calls in func_name so the generated
  // struct name doesn't contain ncc builtins that would re-enter the
  // parser.
  ncc_string_t resolved_name = resolve_ncc_type_calls(func_name);
  pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "struct _%s__kargs { ",
                          resolved_name.data);
  ncc_free(resolved_name.data);

  // Actual param members
  for (int i = 0; i < kw->num_params; i++) {
    pos +=
        (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s %s ; ",
                         kw->params[i].type_text.data, kw->params[i].name.data);
  }

  // Bitfield members for _has_xxx
  for (int i = 0; i < kw->num_params; i++) {
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            "unsigned _has_%s : 1 ; ", kw->params[i].name.data);
  }

  pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "} ;");

  return parse_template(g, "external_declaration", buf);
}

// ============================================================================
// Rewrite parameter_type_list: remove "+" and optional type_name,
// add <vargs_type> *vargs parameter.
// ============================================================================

static void rewrite_vargs_params(ncc_xform_ctx_t *ctx,
                                 ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *fd = find_func_declarator(declarator);
  if (!fd) {
    return;
  }

  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(fd, "parameter_type_list");
  if (!ptl) {
    return;
  }

  // Build a new parameter_type_list.
  // Collect the existing parameter_list text (if any), then append the
  // vargs param.
  ncc_parse_tree_t *pl = ncc_xform_find_child_nt(ptl, "parameter_list");
  ncc_string_t pl_text = {0};
  if (pl) {
    pl_text = ncc_xform_node_to_text(pl);
  }

  // Resolve any typeid/typehash in existing params before interpolation.
  ncc_string_t resolved_pl =
      pl_text.data ? resolve_ncc_type_calls(pl_text.data) : (ncc_string_t){0};
  ncc_free(pl_text.data);

  const char *vt = get_vargs_type(ctx);
  char new_ptl_src[4096];
  if (resolved_pl.data && resolved_pl.u8_bytes > 0) {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "%s , %s * vargs",
             resolved_pl.data, vt);
  } else {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "%s * vargs", vt);
  }
  ncc_free(resolved_pl.data);

  ncc_parse_tree_t *new_ptl =
      parse_template(ctx->grammar, "parameter_type_list", new_ptl_src);
  if (!new_ptl) {
    return;
  }

  // Replace ptl in fd (may be wrapped in a group node).
  int ptl_idx = find_child_index_containing_nt(fd, "parameter_type_list");
  if (ptl_idx >= 0) {
    ncc_xform_set_child(fd, (size_t)ptl_idx, new_ptl);
  }
}

// ============================================================================
// Add kargs parameter to function declarator
// ============================================================================

static void add_kargs_param(ncc_grammar_t *g, ncc_parse_tree_t *declarator,
                            const char *func_name) {
  ncc_parse_tree_t *fd = find_func_declarator(declarator);
  if (!fd) {
    return;
  }

  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(fd, "parameter_type_list");

  ncc_string_t existing_text = {0};
  if (ptl) {
    existing_text = ncc_xform_node_to_text(ptl);
  }

  // Resolve any typeid/typehash in func_name and existing params
  // before interpolation, so the generated text doesn't contain ncc
  // builtins that would re-enter the parser.
  ncc_string_t resolved_name = resolve_ncc_type_calls(func_name);
  ncc_string_t resolved_existing =
      existing_text.data ? resolve_ncc_type_calls(existing_text.data)
                         : (ncc_string_t){0};
  ncc_free(existing_text.data);

  char new_ptl_src[4096];
  if (resolved_existing.data && resolved_existing.u8_bytes > 0) {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "%s , struct _%s__kargs * kargs",
             resolved_existing.data, resolved_name.data);
  } else {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "struct _%s__kargs * kargs",
             resolved_name.data);
  }
  ncc_free(resolved_existing.data);
  ncc_free(resolved_name.data);

  ncc_parse_tree_t *new_ptl =
      parse_template(g, "parameter_type_list", new_ptl_src);
  if (!new_ptl) {
    fprintf(stderr, "ncc: debug: add_kargs_param: template parse failed\n");
    return;
  }

  if (ptl) {
    // Find the direct child of fd that contains parameter_type_list
    // (may be wrapped in a group node due to ? in grammar).
    int ptl_idx = find_child_index_containing_nt(fd, "parameter_type_list");
    if (ptl_idx >= 0) {
      ncc_xform_set_child(fd, (size_t)ptl_idx, new_ptl);
    }
  } else {
    // Function has no params, insert ptl before the closing ")".
    // fd children: direct_declarator "(" ")"
    // Insert new_ptl before the last child.
    size_t nc = ncc_tree_num_children(fd);
    ncc_xform_insert_child(fd, nc - 1, new_ptl);
  }
}

// ============================================================================
// Add opaque kargs parameter (void *kargs) to function declarator
// ============================================================================

static void add_opaque_kargs_param(ncc_grammar_t *g,
                                   ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *fd = find_func_declarator(declarator);
  if (!fd) {
    return;
  }

  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(fd, "parameter_type_list");

  ncc_string_t existing_text = {0};
  if (ptl) {
    existing_text = ncc_xform_node_to_text(ptl);
  }

  // Resolve any typeid/typehash in existing params before interpolation.
  ncc_string_t resolved_existing =
      existing_text.data ? resolve_ncc_type_calls(existing_text.data)
                         : (ncc_string_t){0};
  ncc_free(existing_text.data);

  char new_ptl_src[4096];
  if (resolved_existing.data && resolved_existing.u8_bytes > 0) {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "%s , void * kargs",
             resolved_existing.data);
  } else {
    snprintf(new_ptl_src, sizeof(new_ptl_src), "void * kargs");
  }
  ncc_free(resolved_existing.data);

  ncc_parse_tree_t *new_ptl =
      parse_template(g, "parameter_type_list", new_ptl_src);
  if (!new_ptl) {
    return;
  }

  if (ptl) {
    int ptl_idx = find_child_index_containing_nt(fd, "parameter_type_list");
    if (ptl_idx >= 0) {
      ncc_xform_set_child(fd, (size_t)ptl_idx, new_ptl);
    }
  } else {
    size_t nc = ncc_tree_num_children(fd);
    ncc_xform_insert_child(fd, nc - 1, new_ptl);
  }
}

// ============================================================================
// Remove keyword_clause from function_definition or declaration
// ============================================================================

static void remove_keyword_clause(ncc_parse_tree_t *node) {
  int idx = find_child_index_nt(node, "keyword_clause");
  if (idx >= 0) {
    ncc_xform_remove_child(node, (size_t)idx);
  }
}

// ============================================================================
// Inject body extraction code for kargs
// ============================================================================

static void inject_kargs_body(ncc_grammar_t *g, ncc_parse_tree_t *func_body,
                              ncc_kw_info_t *kw) {
  // Find compound_statement -> block_item_list
  ncc_parse_tree_t *compound =
      ncc_xform_find_child_nt(func_body, "compound_statement");
  if (!compound) {
    return;
  }

  ncc_parse_tree_t *bil = ncc_xform_find_child_nt(compound, "block_item_list");

  // For each keyword param, generate an extraction declaration.
  // Insert them at the start of the block.
  // We need to build block_items and insert into block_item_list.

  // If no block_item_list, we need to create one.
  // compound_statement ::= "{" block_item_list? "}"

  for (int i = kw->num_params - 1; i >= 0; i--) {
    ncc_kw_param_t *p = &kw->params[i];

    char decl_src[2048];
    if (p->default_text.data) {
      snprintf(
          decl_src, sizeof(decl_src),
          "[[maybe_unused]] %s %s = kargs -> _has_%s ? kargs -> %s : ( %s ) ;",
          p->type_text.data, p->name.data, p->name.data, p->name.data,
          p->default_text.data);
    } else {
      snprintf(decl_src, sizeof(decl_src),
               "[[maybe_unused]] %s %s = kargs -> %s ;", p->type_text.data,
               p->name.data, p->name.data);
    }

    ncc_parse_tree_t *block_item = parse_template(g, "block_item", decl_src);
    if (!block_item) {
      continue;
    }

    if (bil) {
      ncc_xform_insert_child(bil, 0, block_item);
    } else {
      // Create block_item_list with this single item.
      ncc_parse_tree_t *children[] = {block_item};
      bil = ncc_xform_make_node_with_children(g, "block_item_list", 0, children,
                                              1);

      // Insert bil into compound_statement before the closing "}".
      size_t nc = ncc_tree_num_children(compound);
      ncc_xform_insert_child(compound, nc - 1, bil);
    }
  }
}

// ============================================================================
// Helpers: find parent external_declaration via parent pointer chain
// ============================================================================

static ncc_parse_tree_t *find_parent_external_decl(ncc_parse_tree_t *node) {
  return ncc_xform_find_ancestor(node, "external_declaration");
}

// ============================================================================
// Insert struct node before external_declaration in TU
// ============================================================================

static void insert_struct_before(ncc_parse_tree_t *ext_decl,
                                 ncc_parse_tree_t *struct_node) {
  if (!ext_decl || !struct_node) {
    return;
  }

  // Get the parent of ext_decl (a group node or translation_unit).
  ncc_nt_node_t pn = ncc_tree_node_value(ext_decl);
  ncc_parse_tree_t *parent = (ncc_parse_tree_t *)pn.parent;
  if (!parent) {
    return;
  }

  int idx = find_child_index(parent, ext_decl);
  if (idx >= 0) {
    ncc_xform_insert_child(parent, (size_t)idx, struct_node);
  }
}

// ============================================================================
// Phase A: Pre-order on function_definition
// ============================================================================

// Find declarator in a declaration node, handling both direct children
// (function_definition) and init_declarator_list > init_declarator > declarator
// (forward declarations).
static ncc_parse_tree_t *find_decl_declarator(ncc_parse_tree_t *node) {
  // Direct child (function_definition style).
  ncc_parse_tree_t *decl = ncc_xform_find_child_nt(node, "declarator");
  if (decl) {
    return decl;
  }

  // init_declarator_list > init_declarator > declarator
  ncc_parse_tree_t *idl = ncc_xform_find_child_nt(node, "init_declarator_list");
  if (idl) {
    ncc_parse_tree_t *id = ncc_xform_find_child_nt(idl, "init_declarator");
    if (id) {
      return ncc_xform_find_child_nt(id, "declarator");
    }
  }

  return nullptr;
}

static ncc_parse_tree_t *xform_funcdef(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *node) {
  // Check for __extension__ wrapper.
  // function_definition ::= attr_spec_seq? declaration_specifiers declarator
  //                         keyword_clause function_body
  //                       | attr_spec_seq? declaration_specifiers declarator
  //                         function_body
  //                       | __extension__ function_definition

  // Handle __extension__ wrapper — find the real function_definition inside.
  ncc_parse_tree_t *real_fd = node;
  size_t nc = ncc_tree_num_children(real_fd);
  if (nc == 2) {
    ncc_parse_tree_t *c0 = ncc_tree_child(real_fd, 0);
    if (c0 && ncc_tree_is_leaf(c0) &&
        ncc_xform_leaf_text_eq(c0, "__extension__")) {
      real_fd = ncc_tree_child(real_fd, 1);
      if (!real_fd || !ncc_xform_nt_name_is(real_fd, "function_definition")) {
        return nullptr;
      }
    }
  }

  ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(real_fd, "declarator");
  if (!declarator) {
    return nullptr;
  }

  bool has_kw = (find_child_index_nt(real_fd, "keyword_clause") >= 0);
  bool has_va = param_list_has_plus(declarator);

  if (!has_kw && !has_va) {
    return nullptr;
  }

  ncc_string_t func_name = extract_resolved_func_name(declarator);
  if (!func_name.data) {
    return nullptr;
  }

  meta_table_t *table = get_meta_table(ctx);
  ncc_func_meta_t *meta = meta_lookup(table, func_name.data);

  // Process keyword args.
  if (has_kw) {
    ncc_parse_tree_t *kw_clause =
        ncc_xform_find_child_nt(real_fd, "keyword_clause");

    // Check for _kargs : opaque
    bool is_opaque = false;
    if (find_leaf_with_text(kw_clause, "opaque")) {
      is_opaque = true;
    }

    if (!is_opaque) {
      // Extract params from keyword_param_list.
      ncc_parse_tree_t *kpl =
          ncc_xform_find_child_nt(kw_clause, "keyword_param_list");
      if (!kpl) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: _kargs clause with no parameters "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
      }

      ncc_kw_param_t *params = nullptr;
      int param_count = 0;
      int param_cap = 0;
      collect_kw_params_from_list(kpl, &params, &param_count, &param_cap);

      if (!meta) {
        meta = meta_insert(table, func_name.data);
      }

      if (!meta->kw) {
        // First time seeing kargs for this function.
        meta->kw = ncc_alloc(ncc_kw_info_t);
        meta->kw->params = params;
        meta->kw->num_params = param_count;
        meta->kw->is_opaque = false;

        // Check that definition has defaults.
        bool has_defaults = true;
        for (int i = 0; i < param_count; i++) {
          if (!params[i].default_text.data) {
            has_defaults = false;
            break;
          }
        }
        meta->kw->defaults_set = has_defaults;

        // Count positional params.
        ncc_parse_tree_t *ptl = find_param_type_list(declarator);
        ncc_parse_tree_t *pl =
            ptl ? ncc_xform_find_child_nt(ptl, "parameter_list") : nullptr;
        meta->kw->num_positional = count_positional_params(pl);
      } else {
        // Subsequent: validate params against existing.
        ncc_kw_info_t *existing = meta->kw;

        if (param_count > 0) {
          // Validate each param.
          for (int i = 0; i < param_count; i++) {
            bool found = false;
            for (int j = 0; j < existing->num_params; j++) {
              if (strcmp(params[i].name.data, existing->params[j].name.data) ==
                  0) {
                found = true;
                // Check default consistency.
                if (params[i].default_text.data &&
                    existing->params[j].default_text.data) {
                  if (strcmp(params[i].default_text.data,
                             existing->params[j].default_text.data) != 0) {
                    fprintf(stderr,
                            "ncc: error: default for '%s' in "
                            "'%s' differs from previous "
                            "declaration\n",
                            params[i].name.data, func_name.data);
                    exit(1);
                  }
                }
                break;
              }
            }
            if (!found) {
              fprintf(stderr,
                      "ncc: error: unknown keyword parameter '%s' "
                      "in '%s'\n",
                      params[i].name.data, func_name.data);
              exit(1);
            }
          }

          // Warn about missing params.
          for (int j = 0; j < existing->num_params; j++) {
            bool found = false;
            for (int i = 0; i < param_count; i++) {
              if (strcmp(params[i].name.data, existing->params[j].name.data) ==
                  0) {
                found = true;
                break;
              }
            }
            if (!found) {
              fprintf(stderr,
                      "ncc: warning: _kargs redeclaration of '%s' "
                      "missing parameter '%s'\n",
                      func_name.data, existing->params[j].name.data);
            }
          }
        }

        // Free the new params — we keep the original.
        for (int i = 0; i < param_count; i++) {
          ncc_free(params[i].name.data);
          ncc_free(params[i].type_text.data);
          ncc_free(params[i].default_text.data);
        }
        ncc_free(params);
      }
    } else {
      // Opaque kargs.
      if (!meta) {
        meta = meta_insert(table, func_name.data);
      }
      if (!meta->kw) {
        meta->kw = ncc_alloc(ncc_kw_info_t);
        meta->kw->is_opaque = true;
      }
    }

    // Emit struct if not already done.
    if (meta->kw && !meta->kw->is_opaque && !meta->kw->struct_emitted) {
      ncc_parse_tree_t *struct_node =
          generate_kargs_struct(ctx->grammar, func_name.data, meta->kw);
      if (struct_node) {
        ncc_parse_tree_t *ext_decl = find_parent_external_decl(node);
        insert_struct_before(ext_decl, struct_node);
        meta->kw->struct_emitted = true;
      }
    }

    // Remove keyword_clause from function_definition.
    remove_keyword_clause(real_fd);
  }

  // Process vargs.
  if (has_va) {
    ncc_parse_tree_t *ptl = find_param_type_list(declarator);
    ncc_vargs_info_t *vi = extract_vargs_info(ptl);

    if (!meta) {
      meta = meta_insert(table, func_name.data);
    }

    if (!meta->va) {
      meta->va = vi;
    } else {
      ncc_free(vi);
    }

    // Rewrite parameter_type_list: remove + and type_name, add
    // <vargs_type> *vargs
    rewrite_vargs_params(ctx, declarator);
  }

  // Add kargs param to signature (after vargs if both).
  if (has_kw && meta->kw) {
    if (meta->kw->is_opaque) {
      add_opaque_kargs_param(ctx->grammar, declarator);
    } else {
      add_kargs_param(ctx->grammar, declarator, func_name.data);
    }
  }

  // For definitions: inject body extraction code (non-opaque only).
  ncc_parse_tree_t *func_body =
      ncc_xform_find_child_nt(real_fd, "function_body");
  if (func_body && meta->kw && !meta->kw->is_opaque) {
    inject_kargs_body(ctx->grammar, func_body, meta->kw);
  }

  ncc_free(func_name.data);
  ctx->nodes_replaced++;
  return node;
}

// Forward declaration — xform_call is defined in Phase B below.
static ncc_parse_tree_t *xform_call(ncc_xform_ctx_t *ctx,
                                    ncc_parse_tree_t *node);

// ============================================================================
// Phase A: Pre-order on declaration (forward decl with _kargs)
// ============================================================================

static ncc_parse_tree_t *xform_decl(ncc_xform_ctx_t *ctx,
                                    ncc_parse_tree_t *node) {
  // declaration ::= declaration_specifiers declarator keyword_clause ";"
  int kw_idx = find_child_index_nt(node, "keyword_clause");

  if (kw_idx < 0) {
    // No keyword_clause. Check for vargs-only forward declarations
    // (e.g. "extern void foo(int x, +);") which still need the "+"
    // replaced with "<vargs_type> *vargs".
    ncc_parse_tree_t *declarator = find_decl_declarator(node);
    if (declarator && param_list_has_plus(declarator)) {
      ncc_string_t func_name = extract_resolved_func_name(declarator);
      if (func_name.data) {
        meta_table_t *table = get_meta_table(ctx);
        ncc_func_meta_t *meta = meta_lookup(table, func_name.data);
        if (!meta) {
          meta = meta_insert(table, func_name.data);
        }

        ncc_parse_tree_t *ptl = find_param_type_list(declarator);
        ncc_vargs_info_t *vi = extract_vargs_info(ptl);
        if (!meta->va) {
          meta->va = vi;
        } else {
          ncc_free(vi);
        }

        rewrite_vargs_params(ctx, declarator);
        ncc_free(func_name.data);
        ctx->nodes_replaced++;
        return node;
      }
    }

    // Check for misparsed function call: the PWZ parser may parse
    // "func(args);" as a declaration when func is an identifier that
    // hasn't been declared as a typedef. If declaration_specifiers
    // contains only an identifier that is in our function metadata
    // table, this is actually a function call — rebuild it.
    meta_table_t *table = get_meta_table(ctx);
    ncc_parse_tree_t *ds =
        ncc_xform_find_child_nt(node, "declaration_specifiers");
    if (ds) {
      const char *id = extract_func_name(ds);
      if (id) {
        ncc_func_meta_t *meta = meta_lookup(table, id);
        if (meta) {
          ncc_string_t full_text = ncc_xform_node_to_text(node);

          size_t len = full_text.u8_bytes;
          while (len > 0 && (full_text.data[len - 1] == ';' ||
                             full_text.data[len - 1] == ' ')) {
            len--;
          }
          full_text.data[len] = '\0';

          ncc_string_t resolved_text = resolve_ncc_type_calls(full_text.data);
          ncc_free(full_text.data);
          ncc_parse_tree_t *call_expr = parse_template(
              ctx->grammar, "postfix_expression", resolved_text.data);
          ncc_free(resolved_text.data);

          if (call_expr) {
            // Apply call-site transform directly since the
            // tree walker won't revisit replacement children.
            ncc_parse_tree_t *xformed = xform_call(ctx, call_expr);
            if (xformed) {
              call_expr = xformed;
            }

            ncc_string_t ct = ncc_xform_node_to_text(call_expr);
            ncc_string_t resolved_ct = resolve_ncc_type_calls(ct.data);
            ncc_free(ct.data);
            char stmt_src[32768];
            snprintf(stmt_src, sizeof(stmt_src), "%s ;", resolved_ct.data);
            ncc_free(resolved_ct.data);

            ncc_parse_tree_t *expr_stmt =
                parse_template(ctx->grammar, "expression_statement", stmt_src);
            if (expr_stmt) {
              ctx->nodes_replaced++;
              return expr_stmt;
            }
          }
        }
      }
    }
    return nullptr;
  }

  ncc_parse_tree_t *declarator = find_decl_declarator(node);
  if (!declarator) {
    return nullptr;
  }

  bool has_va = param_list_has_plus(declarator);

  ncc_string_t func_name = extract_resolved_func_name(declarator);
  if (!func_name.data) {
    return nullptr;
  }

  ncc_parse_tree_t *kw_clause = ncc_tree_child(node, (size_t)kw_idx);

  meta_table_t *table = get_meta_table(ctx);
  ncc_func_meta_t *meta = meta_lookup(table, func_name.data);

  // Check for opaque.
  bool is_opaque = (find_leaf_with_text(kw_clause, "opaque") != nullptr);

  if (!is_opaque) {
    ncc_parse_tree_t *kpl =
        ncc_xform_find_child_nt(kw_clause, "keyword_param_list");
    if (!kpl) {
      uint32_t line, col;
      ncc_xform_first_leaf_pos(node, &line, &col);
      fprintf(stderr,
              "ncc: error: _kargs clause with no parameters "
              "(line %u, col %u)\n",
              line, col);
      exit(1);
    }

    ncc_kw_param_t *params = nullptr;
    int param_count = 0;
    int param_cap = 0;
    collect_kw_params_from_list(kpl, &params, &param_count, &param_cap);

    if (!meta) {
      meta = meta_insert(table, func_name.data);
    }

    if (!meta->kw) {
      meta->kw = ncc_alloc(ncc_kw_info_t);
      meta->kw->params = params;
      meta->kw->num_params = param_count;
      meta->kw->is_opaque = false;

      bool has_defaults = true;
      for (int i = 0; i < param_count; i++) {
        if (!params[i].default_text.data) {
          has_defaults = false;
          break;
        }
      }
      meta->kw->defaults_set = has_defaults;

      ncc_parse_tree_t *ptl = find_param_type_list(declarator);
      ncc_parse_tree_t *pl =
          ptl ? ncc_xform_find_child_nt(ptl, "parameter_list") : nullptr;
      meta->kw->num_positional = count_positional_params(pl);
    } else {
      // Validate.
      ncc_kw_info_t *existing = meta->kw;
      if (param_count > 0) {
        for (int i = 0; i < param_count; i++) {
          bool found = false;
          for (int j = 0; j < existing->num_params; j++) {
            if (strcmp(params[i].name.data, existing->params[j].name.data) ==
                0) {
              found = true;
              if (params[i].default_text.data &&
                  existing->params[j].default_text.data) {
                if (strcmp(params[i].default_text.data,
                           existing->params[j].default_text.data) != 0) {
                  fprintf(stderr,
                          "ncc: error: default for '%s' in "
                          "'%s' differs from previous "
                          "declaration\n",
                          params[i].name.data, func_name.data);
                  exit(1);
                }
              }
              break;
            }
          }
          if (!found) {
            fprintf(stderr,
                    "ncc: error: unknown keyword parameter '%s' "
                    "in '%s'\n",
                    params[i].name.data, func_name.data);
            exit(1);
          }
        }
      }

      for (int i = 0; i < param_count; i++) {
        ncc_free(params[i].name.data);
        ncc_free(params[i].type_text.data);
        ncc_free(params[i].default_text.data);
      }
      ncc_free(params);
    }
  } else {
    if (!meta) {
      meta = meta_insert(table, func_name.data);
    }
    if (!meta->kw) {
      meta->kw = ncc_alloc(ncc_kw_info_t);
      meta->kw->is_opaque = true;
    }
  }

  // Emit struct if needed.
  if (meta->kw && !meta->kw->is_opaque && !meta->kw->struct_emitted) {
    ncc_parse_tree_t *struct_node =
        generate_kargs_struct(ctx->grammar, func_name.data, meta->kw);
    if (struct_node) {
      ncc_parse_tree_t *ext_decl = find_parent_external_decl(node);
      insert_struct_before(ext_decl, struct_node);
      meta->kw->struct_emitted = true;
    }
  }

  // Remove keyword_clause.
  remove_keyword_clause(node);

  // Process vargs if present.
  if (has_va) {
    ncc_parse_tree_t *ptl = find_param_type_list(declarator);
    ncc_vargs_info_t *vi = extract_vargs_info(ptl);
    if (!meta->va) {
      meta->va = vi;
    } else {
      ncc_free(vi);
    }
    rewrite_vargs_params(ctx, declarator);
  }

  // Add kargs param.
  if (meta->kw) {
    if (meta->kw->is_opaque) {
      add_opaque_kargs_param(ctx->grammar, declarator);
    } else {
      add_kargs_param(ctx->grammar, declarator, func_name.data);
    }
  }

  ncc_free(func_name.data);
  ctx->nodes_replaced++;
  return node;
}

// ============================================================================
// Phase B: Post-order on postfix_expression (call sites)
// ============================================================================

// Extract the callee name from a function call postfix_expression.
// The callee is child[0] (which may be a nested postfix_expression or
// primary_expression containing an identifier).
static const char *extract_callee_name(ncc_parse_tree_t *call_node) {
  size_t nc = ncc_tree_num_children(call_node);
  if (nc < 2) {
    return nullptr;
  }
  ncc_parse_tree_t *callee = ncc_tree_child(call_node, 0);
  return extract_func_name(callee);
}

// Check if an argument_expression_list has any keyword_argument children.
static bool has_keyword_args(ncc_parse_tree_t *arg_list) {
  if (!arg_list) {
    return false;
  }
  if (ncc_xform_nt_name_is(arg_list, "keyword_argument")) {
    return true;
  }
  if (ncc_tree_is_leaf(arg_list)) {
    return false;
  }
  size_t nc = ncc_tree_num_children(arg_list);
  for (size_t i = 0; i < nc; i++) {
    if (has_keyword_args(ncc_tree_child(arg_list, i))) {
      return true;
    }
  }
  return false;
}

// Collected argument info.
typedef struct {
  char *name;             // nullptr for positional
  ncc_parse_tree_t *expr; // The expression subtree
} collected_arg_t;

// Flatten argument_expression_list into positional and keyword args.
static void collect_args(ncc_parse_tree_t *node, collected_arg_t **out,
                         int *out_count, int *out_cap) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  if (ncc_xform_nt_name_is(node, "keyword_argument")) {
    // keyword_argument ::= "." identifier "=" assignment_expression
    // Children: "." identifier "=" assignment_expression
    ncc_parse_tree_t *ident_node = ncc_xform_find_child_nt(node, "identifier");
    ncc_parse_tree_t *expr =
        ncc_xform_find_child_nt(node, "assignment_expression");

    if (*out_count >= *out_cap) {
      *out_cap = *out_cap ? *out_cap * 2 : 16;
      *out = ncc_realloc(*out, sizeof(collected_arg_t) * (size_t)*out_cap);
    }

    collected_arg_t *a = &(*out)[*out_count];
    const char *kw_name = ident_node ? extract_func_name(ident_node) : nullptr;
    a->name = strdup(kw_name ? kw_name : "");
    a->expr = expr;
    (*out_count)++;
    return;
  }

  if (ncc_xform_nt_name_is(node, "assignment_expression") ||
      ncc_xform_nt_name_is(node, "conditional_expression")) {
    // Positional arg.
    if (*out_count >= *out_cap) {
      *out_cap = *out_cap ? *out_cap * 2 : 16;
      *out = ncc_realloc(*out, sizeof(collected_arg_t) * (size_t)*out_cap);
    }

    collected_arg_t *a = &(*out)[*out_count];
    a->name = nullptr;
    a->expr = node;
    (*out_count)++;
    return;
  }

  // Recurse into argument_expression_list and group wrappers.
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(node, i);
    if (!c || ncc_tree_is_leaf(c)) {
      continue;
    }
    collect_args(c, out, out_count, out_cap);
  }
}

// Check whether a positional arg's text is a kw_func-generated kargs
// compound literal: & ( struct _<ANY_FUNC>__kargs ) { ... }
// The kw_func literal may be for a different function than the outer callee
// (e.g. ncc_alloc calls kw_func(ncc_plane_init, ...)), so we match the
// generic pattern rather than a specific callee name.
static bool is_kw_func_kargs_literal(ncc_parse_tree_t *expr) {
  if (!expr) {
    return false;
  }

  ncc_string_t text = ncc_xform_node_to_text(expr);
  if (!text.data) {
    return false;
  }

  // Strip leading whitespace.
  const char *p = text.data;
  while (*p == ' ' || *p == '\t') {
    p++;
  }

  // Match: & ( struct _<ident>__kargs ) { ... }
  static const char generic_prefix[] = "& ( struct _";
  bool match = false;

  if (strncmp(p, generic_prefix, sizeof(generic_prefix) - 1) == 0) {
    // Skip past "& ( struct _", then look for "__kargs )"
    const char *q = p + sizeof(generic_prefix) - 1;
    const char *suffix = strstr(q, "__kargs )");
    if (suffix) {
      // Verify everything between "_" and "__kargs" is identifier chars.
      bool valid = (suffix > q);
      for (const char *c = q; c < suffix && valid; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_')) {
          valid = false;
        }
      }
      match = valid;
    }
  }

  ncc_free(text.data);
  return match;
}

// Extract the first positional argument from an argument_expression_list.
// Returns the assignment_expression or conditional_expression node.
static ncc_parse_tree_t *find_first_positional_arg(ncc_parse_tree_t *arg_list) {
  if (!arg_list) {
    return nullptr;
  }
  if (ncc_xform_nt_name_is(arg_list, "assignment_expression") ||
      ncc_xform_nt_name_is(arg_list, "conditional_expression")) {
    return arg_list;
  }
  if (ncc_tree_is_leaf(arg_list)) {
    return nullptr;
  }
  size_t nc = ncc_tree_num_children(arg_list);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(arg_list, i);
    if (!c || ncc_tree_is_leaf(c)) {
      continue;
    }
    // Skip keyword_argument nodes.
    if (ncc_xform_nt_name_is(c, "keyword_argument")) {
      continue;
    }
    if (ncc_xform_nt_name_is(c, "assignment_expression") ||
        ncc_xform_nt_name_is(c, "conditional_expression")) {
      return c;
    }
    // Recurse into argument_expression_list and group wrappers.
    ncc_parse_tree_t *r = find_first_positional_arg(c);
    if (r) {
      return r;
    }
  }
  return nullptr;
}

// Handle kw_func(target_func, .name=val, ...) calls.
// Produces: &(struct _target__kargs){defaults..., ._has_name=1, .name=val}
static ncc_parse_tree_t *xform_kw_func(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *arg_list) {
  // arg0 is the target function name.
  ncc_parse_tree_t *first_arg = find_first_positional_arg(arg_list);
  if (!first_arg) {
    return nullptr;
  }

  // Extract the target function name (an identifier).
  const char *target_name = extract_func_name(first_arg);
  if (!target_name) {
    return nullptr;
  }

  meta_table_t *table = get_meta_table(ctx);
  ncc_func_meta_t *meta = meta_lookup(table, target_name);

  // Collect keyword arguments.
  collected_arg_t *args = nullptr;
  int arg_count = 0;
  int arg_cap = 0;
  collect_args(arg_list, &args, &arg_count, &arg_cap);

  int n_keyword = 0;
  for (int i = 0; i < arg_count; i++) {
    if (args[i].name) {
      n_keyword++;
    }
  }

  if (n_keyword == 0 && !meta) {
    for (int i = 0; i < arg_count; i++) {
      ncc_free(args[i].name);
    }
    ncc_free(args);
    return nullptr;
  }

  // Build: &(struct _target__kargs){defaults..., ._has_name=1, .name=val}
  // Resolve any typeid/typehash in target_name before interpolation.
  ncc_string_t resolved_target = resolve_ncc_type_calls(target_name);
  char literal_src[16384];
  size_t pos = 0;

  pos += (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos,
                          "& ( struct _%s__kargs ) { ", resolved_target.data);
  ncc_free(resolved_target.data);

  // Phase 1: emit defaults from metadata.
  bool first = true;
  if (meta && meta->kw) {
    for (int i = 0; i < meta->kw->num_params; i++) {
      ncc_kw_param_t *p = &meta->kw->params[i];
      if (!p->default_text.data) {
        continue;
      }
      if (!first) {
        pos += (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos,
                                " , ");
      }
      pos +=
          (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos,
                           ". %s = ( %s )", p->name.data, p->default_text.data);
      first = false;
    }
  }

  // Phase 2: emit user-provided keyword arguments (override defaults).
  for (int i = 0; i < arg_count; i++) {
    if (!args[i].name) {
      continue; // Skip positional (arg0 = target function name).
    }
    ncc_string_t val_text = ncc_xform_node_to_text(args[i].expr);
    if (!first) {
      pos +=
          (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos, " , ");
    }
    pos += (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos,
                            ". _has_%s = 1 , . %s = %s", args[i].name,
                            args[i].name, val_text.data);
    first = false;
    ncc_free(val_text.data);
  }

  pos += (size_t)snprintf(literal_src + pos, sizeof(literal_src) - pos, " }");
  literal_src[pos] = '\0';

  // Free collected args.
  for (int i = 0; i < arg_count; i++) {
    ncc_free(args[i].name);
  }
  ncc_free(args);

  // Resolve any embedded typeid/typehash in the text.
  ncc_string_t resolved = resolve_ncc_type_calls(literal_src);

  // &(struct ...){ ... } is a unary_expression (address-of a compound
  // literal), which sits above postfix_expression in the grammar.
  // Parse as assignment_expression (top of expression hierarchy) so the
  // parser can handle the & operator, then return it as the replacement
  // for the postfix_expression node — the tree walker accepts any
  // expression-level node as a replacement.
  ncc_parse_tree_t *replacement =
      parse_template(ctx->grammar, "assignment_expression", resolved.data);
  ncc_free(resolved.data);

  if (replacement) {
    ctx->nodes_replaced++;
  }
  return replacement;
}

static ncc_parse_tree_t *xform_call(ncc_xform_ctx_t *ctx,
                                    ncc_parse_tree_t *node) {
  // postfix_expression ::= postfix_expression "(" argument_expression_list? ")"
  size_t nc = ncc_tree_num_children(node);
  if (nc < 3) {
    return nullptr;
  }

  // Check for "(" — if child[1] is not "(", this isn't a call.
  ncc_parse_tree_t *c1 = ncc_tree_child(node, 1);
  if (!c1 || !ncc_tree_is_leaf(c1) || !ncc_xform_leaf_text_eq(c1, "(")) {
    return nullptr;
  }

  const char *callee = extract_callee_name(node);
  if (!callee) {
    return nullptr;
  }

  // Handle kw_func(target, .name=val, ...) — produces a compound literal.
  if (strcmp(callee, "kw_func") == 0) {
    ncc_parse_tree_t *arg_list =
        ncc_xform_find_child_nt(node, "argument_expression_list");
    return xform_kw_func(ctx, arg_list);
  }

  meta_table_t *table = get_meta_table(ctx);
  ncc_func_meta_t *meta = meta_lookup(table, callee);

  // Find argument_expression_list.
  ncc_parse_tree_t *arg_list =
      ncc_xform_find_child_nt(node, "argument_expression_list");

  bool has_kw_args = has_keyword_args(arg_list);

  if (!meta && !has_kw_args) {
    return nullptr;
  }

  if (!meta && has_kw_args) {
    uint32_t line, col;
    ncc_xform_first_leaf_pos(node, &line, &col);
    fprintf(stderr,
            "ncc: error: keyword argument in call to unknown function "
            "'%s' (line %u, col %u)\n",
            callee, line, col);
    exit(1);
  }

  // Collect all arguments.
  collected_arg_t *args = nullptr;
  int arg_count = 0;
  int arg_cap = 0;
  collect_args(arg_list, &args, &arg_count, &arg_cap);

  // Separate positional and keyword args.
  int n_positional = 0;
  int n_keyword = 0;

  for (int i = 0; i < arg_count; i++) {
    if (!args[i].name) {
      n_positional++;
    } else {
      n_keyword++;
    }
  }

  // If there are no keyword arguments, check whether the call has already
  // been transformed.  A transformed call has exactly the expected number
  // of positional args plus one synthesized arg per vargs/kargs parameter.
  // Vargs-only functions are never skipped here because excess positional
  // args need to be bundled into a vargs compound literal.
  if (n_keyword == 0 && meta) {
    bool already_done = false;

    if (!meta->va && !meta->kw) {
      // Function has no special params — nothing to do.
      already_done = true;
    } else if (meta->kw && !meta->va) {
      // kargs-only: expected = positional + 1 kargs struct.
      int expected = meta->kw->num_positional + 1;
      if (n_positional == expected) {
        already_done = true;
      }
    } else if (meta->va && meta->kw) {
      // vargs+kargs: expected = positional + 1 vargs + 1 kargs.
      // But if the last positional arg is a kw_func-generated kargs
      // compound literal, it looks like a positional arg but is really
      // the kargs — the call still needs transformation.
      int expected = meta->va->num_positional + 2;
      if (n_positional == expected) {
        // Find the last positional arg and check if it's a kw_func
        // kargs literal.  If so, the call is NOT already done.
        ncc_parse_tree_t *last_pos = nullptr;
        int pos_count = 0;
        for (int i = 0; i < arg_count; i++) {
          if (!args[i].name) {
            pos_count++;
            if (pos_count == n_positional) {
              last_pos = args[i].expr;
            }
          }
        }
        if (!last_pos || !is_kw_func_kargs_literal(last_pos)) {
          already_done = true;
        }
      }
    }
    // vargs-only: never skip — excess args need bundling.

    if (already_done) {
      for (int i = 0; i < arg_count; i++) {
        ncc_free(args[i].name);
      }
      ncc_free(args);
      return nullptr;
    }
  }

  // Check for duplicate keyword args.
  for (int i = 0; i < arg_count; i++) {
    if (!args[i].name) {
      continue;
    }
    for (int j = i + 1; j < arg_count; j++) {
      if (!args[j].name) {
        continue;
      }
      if (strcmp(args[i].name, args[j].name) == 0) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: duplicate keyword argument '.%s' in call "
                "to '%s' (line %u, col %u)\n",
                args[i].name, callee, line, col);
        exit(1);
      }
    }
  }

  // Build new argument list text.
  char new_args[16384];
  size_t pos = 0;

  // Determine how many positional args belong to vargs.
  int fixed_positional = n_positional;
  int vargs_count = 0;
  ncc_string_t kw_func_kargs_text = {
      0}; // Non-nullptr .data if last varg is a kw_func kargs literal.

  if (meta->va && meta->va->style == NCC_VARGS_N00B) {
    fixed_positional = meta->va->num_positional;
    vargs_count = n_positional - fixed_positional;
    if (vargs_count < 0) {
      vargs_count = 0;
    }

    // When the function also has kargs, check if the last positional arg
    // in the vargs range is a kw_func-generated kargs compound literal.
    // If so, exclude it from vargs and use it directly as the kargs arg.
    // This applies to both opaque and non-opaque kargs functions.
    if (vargs_count > 0 && meta->kw) {
      // Find the last positional arg.
      ncc_parse_tree_t *last_varg = nullptr;
      int pos_count = 0;
      for (int i = 0; i < arg_count; i++) {
        if (!args[i].name) {
          pos_count++;
          if (pos_count == n_positional) {
            last_varg = args[i].expr;
          }
        }
      }
      if (last_varg && is_kw_func_kargs_literal(last_varg)) {
        kw_func_kargs_text = ncc_xform_node_to_text(last_varg); // ncc_string_t
        vargs_count--;
      }
    }
  }

  // Emit fixed positional args.
  int pos_idx = 0;
  for (int i = 0; i < arg_count && pos_idx < fixed_positional; i++) {
    if (args[i].name) {
      continue;
    }
    ncc_string_t text = ncc_xform_node_to_text(args[i].expr);
    if (pos > 0) {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
    }
    pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, "%s",
                            text.data);
    ncc_free(text.data);
    pos_idx++;
  }

  // Build vargs compound literal.
  if (meta->va && meta->va->style == NCC_VARGS_N00B) {
    if (pos > 0) {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
    }

    // Determine if the vargs have a typed hint (e.g. "ncc_tc_field_t +").
    // If the type is a struct/union (not a pointer), items may be larger
    // than 64 bits and we need to pass pointers to compound literals
    // instead of casting to void *.
    ncc_string_t va_type_text = {0};
    bool va_type_is_large = false;

    if (meta->va->type_node) {
      va_type_text = ncc_xform_node_to_text(meta->va->type_node);
      // Heuristic: if the type text does NOT end with '*', it's
      // a value type that may be larger than a pointer.  We pass
      // a pointer to a compound-literal copy instead.
      if (va_type_text.data) {
        size_t tlen = va_type_text.u8_bytes;
        // Trim trailing spaces.
        while (tlen > 0 && va_type_text.data[tlen - 1] == ' ') {
          tlen--;
        }
        if (tlen > 0 && va_type_text.data[tlen - 1] != '*') {
          va_type_is_large = true;
        }
      }
    }

    if (vargs_count > 0) {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                              "& ( %s ) { . nargs = %d , . cur_ix = 0 , "
                              ". args = ( void * [] ) { ",
                              get_vargs_type(ctx), vargs_count);

      // Collect the vargs (positional args past fixed_positional).
      // Stop at vargs_count to exclude a trailing kw_func kargs
      // literal (if detected above).
      int va_idx = 0;
      int cur_pos = 0;
      for (int i = 0; i < arg_count && va_idx < vargs_count; i++) {
        if (args[i].name) {
          continue;
        }
        if (cur_pos >= fixed_positional) {
          ncc_string_t text = ncc_xform_node_to_text(args[i].expr);
          if (va_idx > 0) {
            pos +=
                (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
          }
          if (va_type_is_large) {
            // Large value type: use a statement expression
            // to capture the value in a temp and take its
            // address so it fits in void *.
            pos +=
                (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                 "({ %s _va_tmp_%d = %s ; "
                                 "( void * ) & _va_tmp_%d ; })",
                                 va_type_text.data, va_idx, text.data, va_idx);
          } else {
            pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                    "( void * ) ( %s )", text.data);
          }
          ncc_free(text.data);
          va_idx++;
        }
        cur_pos++;
      }

      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " } }");
    } else {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                              "& ( %s ) { . nargs = 0 , . cur_ix = 0 , "
                              ". args = ( void * [] ) { 0 } }",
                              get_vargs_type(ctx));
    }

    ncc_free(va_type_text.data);
  }

  // Build kargs argument.
  if (meta->kw && meta->kw->is_opaque) {
    // Opaque kargs: pass the kw_func literal, a forwarded keyword arg,
    // or nullptr.
    if (pos > 0) {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
    }

    if (kw_func_kargs_text.data) {
      // kw_func() built a kargs literal that was among the positional
      // args — pass it through as the opaque kargs pointer.
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, "%s",
                              kw_func_kargs_text.data);
    } else {
      // Check if any keyword args were provided at this call site.
      bool has_kw = false;
      for (int i = 0; i < arg_count; i++) {
        if (args[i].name) {
          has_kw = true;
          break;
        }
      }

      if (has_kw) {
        // Forward the first keyword arg as opaque kargs pointer.
        for (int i = 0; i < arg_count; i++) {
          if (args[i].name) {
            ncc_string_t val_text = ncc_xform_node_to_text(args[i].expr);
            pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                    "%s", val_text.data);
            ncc_free(val_text.data);
            break;
          }
        }
      } else {
        pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                "( ( void * ) 0 )");
      }
    }
  } else if (meta->kw && !meta->kw->is_opaque) {
    if (pos > 0) {
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
    }

    if (kw_func_kargs_text.data) {
      // kw_func() already built the kargs compound literal with the
      // user's overrides baked in — emit it directly.
      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, "%s",
                              kw_func_kargs_text.data);
    } else {
      ncc_string_t resolved_callee = resolve_ncc_type_calls(callee);
      pos +=
          (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                           "& ( struct _%s__kargs ) { ", resolved_callee.data);
      ncc_free(resolved_callee.data);

      // For each kw param: emit user override if present, else default.
      bool first = true;
      for (int i = 0; i < meta->kw->num_params; i++) {
        ncc_kw_param_t *p = &meta->kw->params[i];

        // Check if user provided an override for this param.
        int override_idx = -1;
        for (int j = 0; j < arg_count; j++) {
          if (args[j].name && strcmp(args[j].name, p->name.data) == 0) {
            override_idx = j;
            break;
          }
        }

        if (override_idx >= 0) {
          // User override: set _has_ flag and value.
          ncc_string_t val_text =
              ncc_xform_node_to_text(args[override_idx].expr);
          if (!first) {
            pos +=
                (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
          }
          pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                  ". _has_%s = 1 , . %s = %s", p->name.data,
                                  p->name.data, val_text.data);
          first = false;
          ncc_free(val_text.data);
        } else if (p->default_text.data) {
          // Use default value.
          if (!first) {
            pos +=
                (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " , ");
          }
          pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos,
                                  ". %s = ( %s )", p->name.data,
                                  p->default_text.data);
          first = false;
        }
      }

      pos += (size_t)snprintf(new_args + pos, sizeof(new_args) - pos, " }");
    }
  }

  new_args[pos] = '\0';

  // Build complete call expression.
  ncc_string_t callee_text = ncc_xform_node_to_text(ncc_tree_child(node, 0));

  char call_src[32768];
  if (pos > 0) {
    snprintf(call_src, sizeof(call_src), "%s ( %s )", callee_text.data,
             new_args);
  } else {
    snprintf(call_src, sizeof(call_src), "%s ( )", callee_text.data);
  }
  ncc_free(callee_text.data);

  // Resolve any embedded typeid/typehash calls in the template text.
  ncc_string_t resolved_src = resolve_ncc_type_calls(call_src);
  ncc_parse_tree_t *replacement =
      parse_template(ctx->grammar, "postfix_expression", resolved_src.data);
  ncc_free(resolved_src.data);

  // Free collected args.
  for (int i = 0; i < arg_count; i++) {
    ncc_free(args[i].name);
  }
  ncc_free(args);
  ncc_free(kw_func_kargs_text.data);

  if (replacement) {
    ctx->nodes_replaced++;
    return replacement;
  }

  return nullptr;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_kargs_vargs_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "function_definition", xform_funcdef,
                     "kargs_vargs_funcdef");
  ncc_xform_register(reg, "declaration", xform_decl, "kargs_vargs_decl");
  ncc_xform_register(reg, "postfix_expression", xform_call, "kargs_vargs_call");
}
