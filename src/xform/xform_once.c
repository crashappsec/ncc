// xform_once.c — Transform: `once` function specifier.
//
// `once` makes a function execute its body at most once (thread-safe).
// Subsequent calls return the cached result (or nothing, for void fns).
//
// Registered as pre-order on "translation_unit" with SKIP_CHILDREN.
// We walk external_declaration children ourselves to insert siblings.
//
// For void functions:
//   static int <pfx><name>_once_flag;
//   static void <pfx>once_impl_<name>(void) { <original body> }
//   void <name>(void) {
//       if (__atomic_exchange_n(&<pfx><name>_once_flag,1,__ATOMIC_ACQ_REL)==0){
//           <pfx>once_impl_<name>();
//           __atomic_store_n(&<pfx><name>_once_flag,2,__ATOMIC_RELEASE);
//       } else {
//           while(__atomic_load_n(&<pfx><name>_once_flag,__ATOMIC_ACQUIRE)!=2){}
//       }
//   }
//
// For non-void functions, additionally:
//   static long long <pfx><name>_cached;
//   ... and the wrapper returns (ReturnType)<pfx><name>_cached.
//
// Where <pfx> defaults to "__ncc_" (overridable via once_prefix config).

#include "lib/alloc.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Layout-compatible accessor for ncc_xform_data_t.once_prefix
// ============================================================================

#define _NCC_META_TABLE_SIZE 256

typedef struct {
  char *key;
  void *value;
} _once_meta_entry_t;

typedef struct {
  _once_meta_entry_t entries[_NCC_META_TABLE_SIZE];
} _once_meta_table_t;

typedef struct {
  const char *compiler;
  const char *constexpr_headers;
  _once_meta_table_t func_meta;
  ncc_dict_t option_meta;
  ncc_dict_t option_decls;
  ncc_dict_t generic_struct_decls;
  void *template_reg;
  const char *vargs_type;
  const char *once_prefix;
} _once_xform_data_t;

static const char *get_once_prefix(ncc_xform_ctx_t *ctx) {
  _once_xform_data_t *d = ctx->user_data;
  return d->once_prefix;
}

// ============================================================================
// Helpers: find the declaration_specifier containing "once"
// ============================================================================

// DFS through a subtree looking for a leaf whose text is "once".
// Returns the declaration_specifier ancestor that wraps it, or nullptr.
// We look for: declaration_specifiers -> declaration_specifier ->
//              function_specifier -> "once" leaf
static ncc_parse_tree_t *find_once_leaf(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, "_Once") ? node : nullptr;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *r = find_once_leaf(ncc_tree_child(node, i));
    if (r) {
      return r;
    }
  }
  return nullptr;
}

// Find the declaration_specifier child of decl_specs that contains "once".
static ncc_parse_tree_t *find_once_decl_spec(ncc_parse_tree_t *decl_specs) {
  if (!decl_specs) {
    return nullptr;
  }
  size_t nc = ncc_tree_num_children(decl_specs);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *ds = ncc_tree_child(decl_specs, i);
    if (!ds || ncc_tree_is_leaf(ds)) {
      continue;
    }
    // Each ds is a declaration_specifier.
    // Check if it contains a function_specifier with "once".
    if (find_once_leaf(ds)) {
      return ds;
    }
  }
  return nullptr;
}

// ============================================================================
// Helpers: remove once from declaration_specifiers
// ============================================================================

static void remove_once(ncc_parse_tree_t *decl_specs,
                        ncc_parse_tree_t *once_ds) {
  size_t nc = ncc_tree_num_children(decl_specs);
  for (size_t i = 0; i < nc; i++) {
    if (ncc_tree_child(decl_specs, i) == once_ds) {
      ncc_xform_remove_child(decl_specs, i);
      return;
    }
  }
}

// ============================================================================
// Helpers: extract function name from declarator
// ============================================================================

static const char *extract_func_name(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }
  if (ncc_tree_is_leaf(node)) {
    // Check if this is an IDENTIFIER token.
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

// ============================================================================
// Helpers: check if return type is void
// ============================================================================

// Walk declaration_specifiers looking for a "void" leaf that's inside
// a type_specifier (not a storage_class or function_specifier).
static bool has_void_type(ncc_parse_tree_t *node) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, "void");
  }
  // Skip storage_class_specifier and function_specifier subtrees.
  if (ncc_xform_nt_name_is(node, "storage_class_specifier") ||
      ncc_xform_nt_name_is(node, "function_specifier")) {
    return false;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (has_void_type(ncc_tree_child(node, i))) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// Helpers: collect return type text from declaration_specifiers
// ============================================================================

// Collect leaf text from declaration_specifiers, skipping storage_class
// and function_specifier NTs. This gives us the return type text for
// casting in the wrapper.
static void collect_type_text(ncc_parse_tree_t *node, char *buf, size_t cap,
                              size_t *pos) {
  if (!node) {
    return;
  }
  if (ncc_tree_is_leaf(node)) {
    const char *text = ncc_xform_leaf_text(node);
    if (text) {
      size_t tlen = strlen(text);
      if (*pos + tlen + 2 < cap) {
        if (*pos > 0) {
          buf[(*pos)++] = ' ';
        }
        memcpy(buf + *pos, text, tlen);
        *pos += tlen;
        buf[*pos] = '\0';
      }
    }
    return;
  }
  // Skip storage_class and function_specifier subtrees.
  if (ncc_xform_nt_name_is(node, "storage_class_specifier") ||
      ncc_xform_nt_name_is(node, "function_specifier")) {
    return;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_type_text(ncc_tree_child(node, i), buf, cap, pos);
  }
}

// ============================================================================
// Helpers: find child index
// ============================================================================

static int find_child_index(ncc_parse_tree_t *parent, ncc_parse_tree_t *child) {
  size_t nc = ncc_tree_num_children(parent);
  for (size_t i = 0; i < nc; i++) {
    if (ncc_tree_child(parent, i) == child) {
      return (int)i;
    }
  }
  return -1;
}

// ============================================================================
// Helpers: check if decl_specs already has "static"
// ============================================================================

static bool has_static(ncc_parse_tree_t *decl_specs) {
  if (!decl_specs) {
    return false;
  }
  if (ncc_tree_is_leaf(decl_specs)) {
    return ncc_xform_leaf_text_eq(decl_specs, "static");
  }
  size_t nc = ncc_tree_num_children(decl_specs);
  for (size_t i = 0; i < nc; i++) {
    if (has_static(ncc_tree_child(decl_specs, i))) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// Template parsing helper
// ============================================================================

static ncc_parse_tree_t *parse_template(ncc_grammar_t *g, const char *nt_name,
                                        const char *src) {
  ncc_result_t(ncc_parse_tree_ptr_t) r =
      ncc_xform_parse_template(g, nt_name, src, nullptr);
  if (ncc_result_is_err(r)) {
    fprintf(stderr, "xform_once: template parse failed for '%s': %s\n", nt_name,
            src);
    return nullptr;
  }
  return ncc_result_get(r);
}

// ============================================================================
// Helpers: rename identifier in a declarator subtree (DFS)
// ============================================================================

// Rename the first IDENTIFIER leaf found in a subtree.
// Walks depth-first. When a parent NT contains an IDENTIFIER leaf,
// replaces that leaf with a new token node bearing new_name.
static bool rename_identifier(ncc_parse_tree_t *node, const char *new_name,
                              uint32_t line, uint32_t col) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (!child) {
      continue;
    }
    if (ncc_tree_is_leaf(child)) {
      ncc_token_info_t *tok = ncc_tree_leaf_value(child);
      if (tok && tok->tid == (int32_t)NCC_TOK_IDENTIFIER) {
        ncc_parse_tree_t *new_leaf =
            ncc_xform_make_token_node(NCC_TOK_IDENTIFIER, new_name, line, col);
        ncc_xform_set_child(node, i, new_leaf);
        return true;
      }
    } else {
      if (rename_identifier(child, new_name, line, col)) {
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// Helpers: add "static" to declaration_specifiers
// ============================================================================

// Parses "static int x;" as external_declaration, then extracts the
// "declaration_specifier" node wrapping "static" and inserts it at
// position 0 in the target decl_specs.
static void add_static_to_decl_specs(ncc_grammar_t *g,
                                     ncc_parse_tree_t *decl_specs) {
  if (has_static(decl_specs)) {
    return;
  }

  // Parse a minimal declaration to get a well-formed static decl_spec.
  ncc_parse_tree_t *tmp =
      parse_template(g, "external_declaration", "static int __tmp;");
  if (!tmp) {
    return;
  }

  // Navigate: external_declaration -> declaration -> declaration_specifiers
  //           -> first declaration_specifier (the "static" one)
  ncc_parse_tree_t *decl = ncc_xform_find_child_nt(tmp, "declaration");
  if (!decl) {
    return;
  }
  ncc_parse_tree_t *tmp_specs =
      ncc_xform_find_child_nt(decl, "declaration_specifiers");
  if (!tmp_specs || ncc_tree_num_children(tmp_specs) < 1) {
    return;
  }

  // The first declaration_specifier should be the "static" one.
  ncc_parse_tree_t *static_ds = ncc_tree_child(tmp_specs, 0);
  if (!static_ds) {
    return;
  }

  // Clone and insert at position 0.
  ncc_parse_tree_t *cloned = ncc_xform_clone(static_ds);
  ncc_xform_insert_child(decl_specs, 0, cloned);
}

// ============================================================================
// Build impl function from clone of original
// ============================================================================

static ncc_parse_tree_t *build_impl_function(ncc_xform_ctx_t *ctx,
                                             ncc_parse_tree_t *ext_decl,
                                             const char *func_name,
                                             uint32_t line, uint32_t col) {
  // Clone the entire external_declaration.
  ncc_parse_tree_t *impl_ext = ncc_xform_clone(ext_decl);

  // Get the function_definition inside.
  ncc_parse_tree_t *impl_fd = nullptr;
  size_t nc = ncc_tree_num_children(impl_ext);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(impl_ext, i);
    if (ncc_xform_nt_name_is(c, "function_definition")) {
      impl_fd = c;
      break;
    }
  }
  if (!impl_fd) {
    return impl_ext;
  }

  // Add "static" to declaration_specifiers.
  ncc_parse_tree_t *impl_specs =
      ncc_xform_find_child_nt(impl_fd, "declaration_specifiers");
  if (impl_specs) {
    add_static_to_decl_specs(ctx->grammar, impl_specs);
  }

  // Rename: function name -> <once_prefix>once_impl_<name>
  const char *pfx = get_once_prefix(ctx);
  char impl_name[256];
  snprintf(impl_name, sizeof(impl_name), "%sonce_impl_%s", pfx, func_name);

  ncc_parse_tree_t *impl_decl = ncc_xform_find_child_nt(impl_fd, "declarator");
  if (impl_decl) {
    rename_identifier(impl_decl, impl_name, line, col);
  }

  // For non-void: we need to wrap the body so it stores the return value
  // into <pfx><name>_cached. But this is complex (need to find/modify
  // return statements deep in the body). Instead, the wrapper calls the
  // impl (which keeps its original return type) and stores the result.
  // So impl keeps its original body and return type unchanged.

  return impl_ext;
}

// ============================================================================
// Build wrapper body using template parsing
// ============================================================================

static ncc_parse_tree_t *build_wrapper_body(ncc_xform_ctx_t *ctx,
                                            const char *func_name,
                                            bool void_ret,
                                            const char *ret_type_text) {
  const char *pfx = get_once_prefix(ctx);
  char src[2048];

  // Fast path: if flag==2 (done), skip the exchange entirely.
  // Without this, exchange(flag,1) clobbers the "done" state and
  // causes subsequent callers to spin forever.
  if (void_ret) {
    snprintf(src, sizeof(src),
             "{ if (__atomic_load_n("
             "&%s%s_once_flag, __ATOMIC_ACQUIRE) == 2) { }"
             " else if (__atomic_exchange_n("
             "&%s%s_once_flag, 1, __ATOMIC_ACQ_REL) == 0) {"
             " %sonce_impl_%s();"
             " __atomic_store_n("
             "&%s%s_once_flag, 2, __ATOMIC_RELEASE);"
             "} else {"
             " while (__atomic_load_n("
             "&%s%s_once_flag, __ATOMIC_ACQUIRE) != 2) {}"
             "} }",
             pfx, func_name, pfx, func_name, pfx, func_name, pfx, func_name,
             pfx, func_name);
  } else {
    snprintf(src, sizeof(src),
             "{ if (__atomic_load_n("
             "&%s%s_once_flag, __ATOMIC_ACQUIRE) == 2) { }"
             " else if (__atomic_exchange_n("
             "&%s%s_once_flag, 1, __ATOMIC_ACQ_REL) == 0) {"
             " %s%s_cached = (long long)%sonce_impl_%s();"
             " __atomic_store_n("
             "&%s%s_once_flag, 2, __ATOMIC_RELEASE);"
             "} else {"
             " while (__atomic_load_n("
             "&%s%s_once_flag, __ATOMIC_ACQUIRE) != 2) {}"
             "}"
             " return (%s)%s%s_cached; }",
             pfx, func_name, pfx, func_name, pfx, func_name, pfx, func_name,
             pfx, func_name, pfx, func_name, ret_type_text, pfx, func_name);
  }

  // Parse as compound_statement.
  ncc_parse_tree_t *compound =
      parse_template(ctx->grammar, "compound_statement", src);
  if (!compound) {
    return nullptr;
  }

  // Wrap in function_body NT.
  ncc_parse_tree_t *children[] = {compound};
  ncc_parse_tree_t *fb = ncc_xform_make_node_with_children(
      ctx->grammar, "function_body", 0, children, 1);
  return fb;
}

// ============================================================================
// Main pre-order transform on translation_unit
// ============================================================================

// Check if a node is a group wrapper (from BNF +, *, ?).
static bool is_group_node(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.group_top;
}

// Growable array of parse tree pointers.
typedef struct {
  ncc_parse_tree_t **items;
  size_t len;
  size_t cap;
} ptrvec_t;

static void ptrvec_init(ptrvec_t *v, size_t init_cap) {
  v->cap = init_cap > 0 ? init_cap : 64;
  v->len = 0;
  v->items = ncc_alloc_array(ncc_parse_tree_t *, v->cap);
}

static void ptrvec_push(ptrvec_t *v, ncc_parse_tree_t *p) {
  if (v->len >= v->cap) {
    v->cap *= 2;
    v->items = ncc_realloc(v->items, v->cap * sizeof(ncc_parse_tree_t *));
  }
  v->items[v->len++] = p;
}

// Recursively collect all non-group children from a (possibly nested)
// group tree into a flat array.
static void flatten_group(ncc_parse_tree_t *node, ptrvec_t *out) {
  if (!node) {
    return;
  }
  if (ncc_tree_is_leaf(node)) {
    ptrvec_push(out, node);
    return;
  }
  if (is_group_node(node)) {
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
      flatten_group(ncc_tree_child(node, i), out);
    }
  } else {
    ptrvec_push(out, node);
  }
}

static ncc_parse_tree_t *
xform_once_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
              [[maybe_unused]] ncc_xform_control_t *control) {
  // The BNF `<translation_unit> ::= <external_declaration>+` creates a
  // right-recursive chain of group wrapper nodes. Flatten them into a
  // single array so we can scan and insert siblings easily.
  ptrvec_t flat;
  ptrvec_init(&flat, 256);

  size_t tu_nc = ncc_tree_num_children(tu);
  for (size_t i = 0; i < tu_nc; i++) {
    flatten_group(ncc_tree_child(tu, i), &flat);
  }

  bool changed = false;

  // Process the flat list, possibly inserting new nodes.
  for (size_t i = 0; i < flat.len; i++) {
    ncc_parse_tree_t *ext_decl = flat.items[i];
    if (!ext_decl || ncc_tree_is_leaf(ext_decl)) {
      continue;
    }

    // external_declaration wraps function_definition or declaration.
    ncc_parse_tree_t *inner = nullptr;
    size_t inner_nc = ncc_tree_num_children(ext_decl);
    for (size_t j = 0; j < inner_nc; j++) {
      ncc_parse_tree_t *c = ncc_tree_child(ext_decl, j);
      if (c && !ncc_tree_is_leaf(c)) {
        inner = c;
        break;
      }
    }
    if (!inner) {
      continue;
    }

    if (ncc_xform_nt_name_is(inner, "function_definition")) {
      ncc_parse_tree_t *decl_specs =
          ncc_xform_find_child_nt(inner, "declaration_specifiers");
      if (!decl_specs) {
        continue;
      }

      ncc_parse_tree_t *once_ds = find_once_decl_spec(decl_specs);
      if (!once_ds) {
        continue;
      }

      // --- Found a once function definition ---
      ncc_parse_tree_t *declarator =
          ncc_xform_find_child_nt(inner, "declarator");
      ncc_parse_tree_t *func_body =
          ncc_xform_find_child_nt(inner, "function_body");
      if (!declarator || !func_body) {
        continue;
      }

      const char *fname = extract_func_name(declarator);
      if (!fname) {
        continue;
      }

      bool void_ret = has_void_type(decl_specs);

      uint32_t line, col;
      ncc_xform_first_leaf_pos(inner, &line, &col);

      // 1. Remove 'once' from declaration_specifiers.
      remove_once(decl_specs, once_ds);

      // 2. Insert static flag declaration before this function.
      const char *pfx = get_once_prefix(ctx);
      char buf[512];
      snprintf(buf, sizeof(buf), "static int %s%s_once_flag;", pfx, fname);
      ncc_parse_tree_t *flag_node =
          parse_template(ctx->grammar, "external_declaration", buf);
      if (flag_node) {
        ptrvec_push(&flat, nullptr); // grow
        memmove(&flat.items[i + 1], &flat.items[i],
                (flat.len - 1 - i) * sizeof(ncc_parse_tree_t *));
        flat.items[i] = flag_node;
        i++;
      }

      // 3. If non-void: insert cached result declaration.
      if (!void_ret) {
        snprintf(buf, sizeof(buf), "static long long %s%s_cached;", pfx, fname);
        ncc_parse_tree_t *cached_node =
            parse_template(ctx->grammar, "external_declaration", buf);
        if (cached_node) {
          ptrvec_push(&flat, nullptr);
          memmove(&flat.items[i + 1], &flat.items[i],
                  (flat.len - 1 - i) * sizeof(ncc_parse_tree_t *));
          flat.items[i] = cached_node;
          i++;
        }
      }

      // 4. Build impl function (clone of original, add static, rename).
      ncc_parse_tree_t *impl_ext =
          build_impl_function(ctx, ext_decl, fname, line, col);
      if (impl_ext) {
        ptrvec_push(&flat, nullptr);
        memmove(&flat.items[i + 1], &flat.items[i],
                (flat.len - 1 - i) * sizeof(ncc_parse_tree_t *));
        flat.items[i] = impl_ext;
        i++;
      }

      // 5. Extract return type text for cast in wrapper.
      char ret_type[256] = {0};
      size_t rpos = 0;
      collect_type_text(decl_specs, ret_type, sizeof(ret_type), &rpos);
      if (rpos == 0) {
        strcpy(ret_type, "int");
      }

      // 6. Replace original function body with wrapper body.
      ncc_parse_tree_t *wrapper_fb =
          build_wrapper_body(ctx, fname, void_ret, ret_type);
      if (wrapper_fb) {
        int body_idx = find_child_index(inner, func_body);
        if (body_idx >= 0) {
          ncc_xform_set_child(inner, (size_t)body_idx, wrapper_fb);
        }
      }

      changed = true;
      ctx->nodes_replaced++;
    } else if (ncc_xform_nt_name_is(inner, "declaration")) {
      ncc_parse_tree_t *decl_specs =
          ncc_xform_find_child_nt(inner, "declaration_specifiers");
      if (!decl_specs) {
        continue;
      }

      ncc_parse_tree_t *once_ds = find_once_decl_spec(decl_specs);
      if (!once_ds) {
        continue;
      }

      // Check if this is a function prototype (has a function
      // declarator) or a variable declaration. _Once on a
      // variable is an error.
      ncc_parse_tree_t *declarator =
          ncc_xform_find_child_nt(inner, "declarator");
      if (!declarator) {
        declarator = ncc_xform_find_child_nt(inner, "init_declarator_list");
      }
      bool is_func_proto = false;
      if (declarator) {
        // A function prototype has a direct_declarator with
        // "(parameter_type_list)" — check for "(" leaf.
        ncc_string_t text = ncc_xform_node_to_text(declarator);
        if (text.data && strchr(text.data, '(')) {
          is_func_proto = true;
        }
        ncc_free(text.data);
      }

      if (!is_func_proto) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(inner, &line, &col);
        fprintf(stderr,
                "ncc: error: '_Once' can only be applied to "
                "functions, not variable declarations "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
      }

      // Function prototype: just strip 'once'.
      remove_once(decl_specs, once_ds);
      changed = true;
      ctx->nodes_replaced++;
    }
  }

  if (!changed) {
    ncc_free(flat.items);
    return nullptr;
  }

  // Rebuild: replace tu's children with a single flat group node
  // containing all the external_declarations (including new ones).
  // Create a new group node with group_top=true.
  ncc_nt_node_t gpn = {0};
  gpn.name = ncc_string_from_cstr("$$group_once");
  gpn.id = (1 << 28); // NCC_GROUP_ID
  gpn.group_top = true;

  ncc_parse_tree_t *new_group =
      ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, gpn);

  for (size_t i = 0; i < flat.len; i++) {
    if (flat.items[i]) {
      ncc_tree_add_child(new_group, flat.items[i]);
    }
  }

  ncc_free(flat.items);

  // Replace tu's children with the single new group node.
  ncc_tree_replace_children(tu, ncc_alloc_array(ncc_parse_tree_t *, 1), 1);
  ncc_tree_set_child(tu, 0, new_group);

  return tu;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_once_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register_pre(reg, "translation_unit", xform_once_tu, "once");
}
