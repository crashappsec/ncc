// xform_once.c — Transform: `_Once` function specifier.
//
// `_Once` makes a function execute its body at most once (thread-safe).
// Subsequent calls return the cached result (or nothing, for void fns).
//
// Uses C23 stdatomic: _Atomic int flag, atomic_load/atomic_fetch_or/atomic_store.
// The original function body is moved to a static __internal_<name> helper;
// the public wrapper uses a three-state flag (0=uncalled, 1=running, 2=done).
//
// Two templates ("once" and "once_void") are registered in ncc.c and
// instantiated here with $0..$7 substitutions.
//
// Registered as pre-order on "translation_unit" with SKIP_CHILDREN.
// We walk external_declaration children ourselves to insert siblings.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers: find the declaration_specifier containing "_Once"
// ============================================================================

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
    if (find_once_leaf(ds)) {
      return ds;
    }
  }
  return nullptr;
}

// ============================================================================
// Helpers: remove _Once from declaration_specifiers
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

// Get parameter list text from the declarator subtree.
static char *collect_param_list_text(ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *dd = ncc_xform_find_child_nt(declarator, "direct_declarator");
  if (!dd) {
    dd = declarator;
  }

  // Search for parameter_type_list at this level and one level deeper.
  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(dd, "parameter_type_list");
  if (!ptl) {
    size_t nc = ncc_tree_num_children(dd);
    for (size_t i = 0; i < nc; i++) {
      ncc_parse_tree_t *c = ncc_tree_child(dd, i);
      if (c && !ncc_tree_is_leaf(c)) {
        ptl = ncc_xform_find_child_nt(c, "parameter_type_list");
        if (ptl) {
          break;
        }
      }
    }
  }

  if (ptl) {
    ncc_string_t text = ncc_xform_node_to_text(ptl);
    if (text.data) {
      char *result = ncc_alloc_size(1, text.u8_bytes + 1);
      memcpy(result, text.data, text.u8_bytes);
      result[text.u8_bytes] = '\0';
      ncc_free(text.data);
      return result;
    }
  }

  char *result = ncc_alloc_size(1, 5);
  memcpy(result, "void", 5);
  return result;
}

// Extract just the parameter names, joined with ", ".
static char *collect_param_names(ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *dd = ncc_xform_find_child_nt(declarator, "direct_declarator");
  if (!dd) {
    dd = declarator;
  }

  ncc_parse_tree_t *ptl = nullptr;
  ptl = ncc_xform_find_child_nt(dd, "parameter_type_list");
  if (!ptl) {
    size_t nc = ncc_tree_num_children(dd);
    for (size_t i = 0; i < nc; i++) {
      ncc_parse_tree_t *c = ncc_tree_child(dd, i);
      if (c && !ncc_tree_is_leaf(c)) {
        ptl = ncc_xform_find_child_nt(c, "parameter_type_list");
        if (ptl) {
          break;
        }
      }
    }
  }

  if (!ptl) {
    char *result = ncc_alloc_size(1, 1);
    result[0] = '\0';
    return result;
  }

  // Check for (void).
  ncc_string_t ptl_text = ncc_xform_node_to_text(ptl);
  if (ptl_text.data && strcmp(ptl_text.data, "void") == 0) {
    ncc_free(ptl_text.data);
    char *result = ncc_alloc_size(1, 1);
    result[0] = '\0';
    return result;
  }
  ncc_free(ptl_text.data);

  // DFS to find parameter_declaration nodes and extract identifiers.
  ncc_buffer_t *buf = ncc_buffer_empty();
  bool first = true;

  struct { ncc_parse_tree_t *node; } stack[64];
  int sp = 0;
  stack[sp++].node = ptl;

  while (sp > 0) {
    ncc_parse_tree_t *n = stack[--sp].node;
    if (!n || ncc_tree_is_leaf(n)) {
      continue;
    }

    if (ncc_xform_nt_name_is(n, "parameter_declaration")) {
      ncc_parse_tree_t *pd_decl = ncc_xform_find_child_nt(n, "declarator");
      if (pd_decl) {
        const char *name = extract_func_name(pd_decl);
        if (name) {
          if (!first) {
            ncc_buffer_puts(buf, ", ");
          }
          ncc_buffer_puts(buf, name);
          first = false;
        }
      }
      continue;
    }

    size_t nc = ncc_tree_num_children(n);
    for (size_t i = nc; i > 0; i--) {
      if (sp < 64) {
        stack[sp++].node = ncc_tree_child(n, i - 1);
      }
    }
  }

  return ncc_buffer_take(buf);
}

// Get the function body text (compound_statement including braces).
static char *collect_body_text(ncc_parse_tree_t *func_body) {
  ncc_parse_tree_t *compound =
      ncc_xform_find_child_nt(func_body, "compound_statement");
  if (!compound) {
    compound = func_body;
  }
  ncc_string_t text = ncc_xform_node_to_text(compound);
  if (text.data) {
    char *result = ncc_alloc_size(1, text.u8_bytes + 1);
    memcpy(result, text.data, text.u8_bytes);
    result[text.u8_bytes] = '\0';
    ncc_free(text.data);
    return result;
  }
  char *result = ncc_alloc_size(1, 4);
  memcpy(result, "{ }", 4);
  return result;
}

// ============================================================================
// Flattening infrastructure
// ============================================================================

static bool is_group_node(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.group_top;
}

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

static void flatten_tu(ncc_parse_tree_t *tu, ptrvec_t *out) {
  size_t nc = ncc_tree_num_children(tu);
  for (size_t i = 0; i < nc; i++) {
    flatten_group(ncc_tree_child(tu, i), out);
  }
}

// ============================================================================
// Main pre-order transform on translation_unit
// ============================================================================

static ncc_parse_tree_t *
xform_once_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
              [[maybe_unused]] ncc_xform_control_t *control) {
  ptrvec_t flat;
  ptrvec_init(&flat, 256);

  size_t tu_nc = ncc_tree_num_children(tu);
  for (size_t i = 0; i < tu_nc; i++) {
    flatten_group(ncc_tree_child(tu, i), &flat);
  }

  bool changed = false;

  for (size_t i = 0; i < flat.len; i++) {
    ncc_parse_tree_t *ext_decl = flat.items[i];
    if (!ext_decl || ncc_tree_is_leaf(ext_decl)) {
      continue;
    }

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

      // --- Found a _Once function definition ---
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

      bool void_ret = ncc_xform_has_void_type(decl_specs);
      ncc_xform_data_t *xdata = ncc_xform_get_data(ctx);
      const char *pfx = xdata->once_prefix;

      // Build template arguments.
      char *base_type = ncc_xform_collect_base_type(decl_specs);
      char *quals = ncc_xform_collect_qualifiers(decl_specs, "_Once");
      char *params = collect_param_list_text(declarator);
      char *param_names = collect_param_names(declarator);
      char *body = collect_body_text(func_body);

      // Build prefixed flag/cache names.
      ncc_buffer_t *flag_buf = ncc_buffer_empty();
      ncc_buffer_puts(flag_buf, pfx);
      ncc_buffer_puts(flag_buf, fname);
      ncc_buffer_puts(flag_buf, "_once_flag");
      char *flag_name = ncc_buffer_take(flag_buf);

      char *cache_name = nullptr;
      if (!void_ret) {
        ncc_buffer_t *cache_buf = ncc_buffer_empty();
        ncc_buffer_puts(cache_buf, pfx);
        ncc_buffer_puts(cache_buf, fname);
        ncc_buffer_puts(cache_buf, "_once_cache");
        cache_name = ncc_buffer_take(cache_buf);
      }

      // Template slots:
      //   $0 = func name
      //   $1 = base return type
      //   $2 = qualifiers (may be empty — but template handles it)
      //   $3 = param list text
      //   $4 = param names
      //   $5 = body text { ... }
      //   $6 = flag variable name
      //   $7 = cache variable name (non-void only)
      ncc_template_registry_t *tmpl_reg = xdata->template_reg;
      ncc_result_t(ncc_parse_tree_ptr_t) r;

      if (void_ret) {
        const char *args[] = {fname, base_type, quals, params,
                              param_names, body, flag_name};
        r = ncc_template_instantiate(tmpl_reg, "once_void", args, 7);
      } else {
        const char *args[] = {fname, base_type, quals, params,
                              param_names, body, flag_name, cache_name};
        r = ncc_template_instantiate(tmpl_reg, "once", args, 8);
      }

      ncc_free(base_type);
      ncc_free(quals);
      ncc_free(params);
      ncc_free(param_names);
      ncc_free(body);
      ncc_free(flag_name);
      ncc_free(cache_name);

      if (ncc_result_is_err(r)) {
        fprintf(stderr, "xform_once: template instantiation failed for '%s'\n",
                fname);
        continue;
      }

      ncc_parse_tree_t *new_tu = ncc_result_get(r);

      // Flatten the new TU and splice into the flat list.
      ptrvec_t new_items;
      ptrvec_init(&new_items, 16);
      flatten_tu(new_tu, &new_items);

      if (new_items.len > 0) {
        size_t new_total = flat.len - 1 + new_items.len;
        ncc_parse_tree_t **new_arr =
            ncc_alloc_array(ncc_parse_tree_t *, new_total);

        if (i > 0) {
          memcpy(new_arr, flat.items, i * sizeof(ncc_parse_tree_t *));
        }
        memcpy(new_arr + i, new_items.items,
               new_items.len * sizeof(ncc_parse_tree_t *));
        size_t after = flat.len - i - 1;
        if (after > 0) {
          memcpy(new_arr + i + new_items.len, flat.items + i + 1,
                 after * sizeof(ncc_parse_tree_t *));
        }

        ncc_free(flat.items);
        flat.items = new_arr;
        flat.len = new_total;
        flat.cap = new_total;

        i += new_items.len - 1;
      }

      ncc_free(new_items.items);
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

      ncc_parse_tree_t *declarator =
          ncc_xform_find_child_nt(inner, "declarator");
      if (!declarator) {
        declarator = ncc_xform_find_child_nt(inner, "init_declarator_list");
      }
      bool is_func_proto = false;
      if (declarator) {
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

      remove_once(decl_specs, once_ds);
      changed = true;
      ctx->nodes_replaced++;
    }
  }

  if (!changed) {
    ncc_free(flat.items);
    return nullptr;
  }

  ncc_nt_node_t gpn = {0};
  gpn.name = ncc_string_from_cstr("$$group_once");
  gpn.id = (1 << 28);
  gpn.group_top = true;

  ncc_parse_tree_t *new_group =
      ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, gpn);

  for (size_t i = 0; i < flat.len; i++) {
    if (flat.items[i]) {
      ncc_tree_add_child(new_group, flat.items[i]);
    }
  }

  ncc_free(flat.items);

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
