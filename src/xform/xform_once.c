// xform_once.c — Transform: `_Once` function specifier.
//
// `_Once` makes a function execute its body at most once (thread-safe).
// Subsequent calls return the cached result (or nothing, for void fns).
//
// parse_once_replacement() assembles parseable C source for a public wrapper,
// a static body helper, and optional result cache. The wrapper uses compiler
// __atomic builtins with a three-state flag (0=uncalled, 1=running, 2=done).
//
// Registered as pre-order on "translation_unit" with SKIP_CHILDREN.
// We walk external_declaration children ourselves to insert siblings.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
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
// Helpers: extract declaration metadata from declarators
// ============================================================================

static bool is_ident_start_char(char c) {
  unsigned char uc = (unsigned char)c;
  return isalpha(uc) || c == '_';
}

static bool is_ident_char(char c) {
  unsigned char uc = (unsigned char)c;
  return isalnum(uc) || c == '_';
}

static const char *skip_spaces(const char *p) {
  while (*p && isspace((unsigned char)*p)) {
    p++;
  }
  return p;
}

static bool word_eq(const char *start, size_t len, const char *word) {
  return strlen(word) == len && memcmp(start, word, len) == 0;
}

static bool is_ms_calling_convention(const char *start, size_t len) {
  return word_eq(start, len, "__cdecl") ||
         word_eq(start, len, "__fastcall") ||
         word_eq(start, len, "__stdcall") ||
         word_eq(start, len, "__thiscall") ||
         word_eq(start, len, "__vectorcall");
}

static const char *skip_quoted(const char *p) {
  char quote = *p++;

  while (*p) {
    if (*p == '\\' && p[1]) {
      p += 2;
      continue;
    }
    if (*p == quote) {
      return p + 1;
    }
    p++;
  }

  return p;
}

static const char *skip_balanced_text(const char *p) {
  int depth = 0;

  while (*p) {
    if (*p == '"' || *p == '\'') {
      p = skip_quoted(p);
      continue;
    }
    if (*p == '(') {
      depth++;
    } else if (*p == ')') {
      depth--;
      p++;
      if (depth <= 0) {
        return p;
      }
      continue;
    }
    p++;
  }

  return p;
}

static char *strip_declarator_attributes(const char *text) {
  ncc_buffer_t *buf = ncc_buffer_empty();
  const char *p = text;

  while (*p) {
    if (is_ident_start_char(*p)) {
      const char *start = p++;
      while (is_ident_char(*p)) {
        p++;
      }
      size_t len = (size_t)(p - start);

      if (word_eq(start, len, "__attribute__") ||
          word_eq(start, len, "__attribute") ||
          word_eq(start, len, "__declspec")) {
        p = skip_spaces(p);
        if (*p == '(') {
          p = skip_balanced_text(p);
        }
        continue;
      }
      if (is_ms_calling_convention(start, len)) {
        continue;
      }

      ncc_buffer_append(buf, start, len);
      continue;
    }

    if (p[0] == '[' && p[1] == '[') {
      p += 2;
      while (*p && !(p[0] == ']' && p[1] == ']')) {
        p++;
      }
      if (*p) {
        p += 2;
      }
      continue;
    }

    ncc_buffer_putc(buf, *p++);
  }

  return ncc_buffer_take(buf);
}

static size_t find_last_top_level_param_open(const char *text) {
  int depth = 0;
  size_t last = (size_t)-1;

  for (size_t i = 0; text[i]; i++) {
    if (text[i] == '"' || text[i] == '\'') {
      const char *next = skip_quoted(text + i);
      i = (size_t)(next - text);
      if (i > 0) {
        i--;
      }
      continue;
    }
    if (text[i] == '(') {
      if (depth == 0) {
        last = i;
      }
      depth++;
    } else if (text[i] == ')' && depth > 0) {
      depth--;
    }
  }

  return last;
}

static char *copy_last_identifier_before(const char *text, size_t end) {
  size_t i = end;

  while (i > 0) {
    while (i > 0 && !is_ident_char(text[i - 1])) {
      i--;
    }
    size_t ident_end = i;
    while (i > 0 && is_ident_char(text[i - 1])) {
      i--;
    }
    if (ident_end > i && is_ident_start_char(text[i])) {
      size_t len = ident_end - i;
      char *name = ncc_alloc_size(1, len + 1);
      memcpy(name, text + i, len);
      name[len] = '\0';
      return name;
    }
  }

  return nullptr;
}

static bool is_type_keyword(const char *name) {
  static const char *keywords[] = {
      "void", "char", "short", "int", "long", "float", "double",
      "signed", "unsigned", "const", "volatile", "restrict", "static",
      "struct", "union", "enum", "_Atomic", "_Bool", "bool", "c_va",
      nullptr,
  };

  for (int i = 0; keywords[i]; i++) {
    if (strcmp(name, keywords[i]) == 0) {
      return true;
    }
  }

  return false;
}

static char *extract_func_name(ncc_parse_tree_t *declarator) {
  ncc_string_t text = ncc_xform_node_to_text(declarator);
  if (!text.data) {
    return nullptr;
  }

  char *clean = strip_declarator_attributes(text.data);
  ncc_free(text.data);

  size_t param_open = find_last_top_level_param_open(clean);
  size_t name_end = param_open == (size_t)-1 ? strlen(clean) : param_open;
  char *name = copy_last_identifier_before(clean, name_end);

  ncc_free(clean);
  return name;
}

static char *collect_return_modifier_text(ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *ptr = ncc_xform_find_child_nt(declarator, "pointer");
  if (!ptr) {
    char *result = ncc_alloc_size(1, 1);
    result[0] = '\0';
    return result;
  }

  ncc_string_t text = ncc_xform_node_to_text(ptr);
  if (!text.data) {
    char *result = ncc_alloc_size(1, 1);
    result[0] = '\0';
    return result;
  }

  char *clean = strip_declarator_attributes(text.data);
  ncc_free(text.data);
  return clean;
}

static char *join_return_type(const char *base_type, const char *modifier) {
  ncc_buffer_t *buf = ncc_buffer_empty();

  ncc_buffer_puts(buf, base_type);
  if (modifier && modifier[0]) {
    ncc_buffer_putc(buf, ' ');
    ncc_buffer_puts(buf, modifier);
  }

  return ncc_buffer_take(buf);
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
static void append_param_name(ncc_buffer_t *buf, bool *first,
                              const char *param, size_t len) {
  char *part = ncc_alloc_size(1, len + 1);
  memcpy(part, param, len);
  part[len] = '\0';

  char *clean = strip_declarator_attributes(part);
  ncc_free(part);

  char *name = copy_last_identifier_before(clean, strlen(clean));
  if (name && !is_type_keyword(name)) {
    if (!*first) {
      ncc_buffer_puts(buf, ", ");
    }
    ncc_buffer_puts(buf, name);
    *first = false;
  }

  ncc_free(name);
  ncc_free(clean);
}

static char *collect_param_names(ncc_parse_tree_t *declarator) {
  char *params = collect_param_list_text(declarator);
  ncc_buffer_t *buf = ncc_buffer_empty();
  bool first = true;
  int depth = 0;
  size_t start = 0;
  size_t len = strlen(params);

  if (strcmp(params, "void") == 0) {
    ncc_free(params);
    return ncc_buffer_take(buf);
  }

  for (size_t i = 0; i <= len; i++) {
    char c = params[i];
    if (c == '(' || c == '[' || c == '{') {
      depth++;
      continue;
    }
    if ((c == ')' || c == ']' || c == '}') && depth > 0) {
      depth--;
      continue;
    }
    if ((c == ',' && depth == 0) || c == '\0') {
      append_param_name(buf, &first, params + start, i - start);
      start = i + 1;
    }
  }

  ncc_free(params);
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

enum {
  ONCE_GENERATED_STATE_UNCALLED = 0,
  ONCE_GENERATED_STATE_RUNNING = 1,
  ONCE_GENERATED_STATE_DONE = 2,
};

static void emit_once_declarations(ncc_buffer_t *src, bool void_ret,
                                   const char *return_type, const char *params,
                                   const char *flag_name,
                                   const char *cache_name,
                                   const char *internal_name) {
  ncc_buffer_printf(src, "static int %s = %d;\n", flag_name,
                    ONCE_GENERATED_STATE_UNCALLED);
  if (!void_ret) {
    ncc_buffer_printf(src, "static %s %s;\n", return_type, cache_name);
  }
  ncc_buffer_printf(src, "static %s %s(%s);\n",
                    void_ret ? "void" : return_type, internal_name, params);
}

static void emit_once_wrapper_header(ncc_buffer_t *src, bool void_ret,
                                     const char *fname,
                                     const char *return_type,
                                     const char *quals,
                                     const char *params) {
  if (quals && quals[0]) {
    ncc_buffer_printf(src, "%s ", quals);
  }
  ncc_buffer_printf(src, "%s %s(%s) {\n",
                    void_ret ? "void" : return_type, fname, params);
}

static void emit_once_state_switch(ncc_buffer_t *src,
                                   const char *flag_name) {
  ncc_buffer_printf(src,
                    "int expected = __atomic_load_n(&%s, "
                    "__ATOMIC_ACQUIRE);\n",
                    flag_name);
  ncc_buffer_puts(src, "switch (expected) {\n");
}

static void emit_once_done_path(ncc_buffer_t *src, bool void_ret,
                                const char *cache_name) {
  ncc_buffer_printf(src, "case %d:\n", ONCE_GENERATED_STATE_DONE);
  if (void_ret) {
    ncc_buffer_puts(src, "return;\n");
  } else {
    ncc_buffer_printf(src, "return %s;\n", cache_name);
  }
}

static void emit_once_wait_path(ncc_buffer_t *src, bool void_ret,
                                const char *flag_name,
                                const char *cache_name) {
  ncc_buffer_printf(src, "case %d:\nwait_for_first_in:\n",
                    ONCE_GENERATED_STATE_RUNNING);
  ncc_buffer_printf(src,
                    "while (__atomic_load_n(&%s, __ATOMIC_ACQUIRE) != %d) "
                    "{ }\n",
                    flag_name, ONCE_GENERATED_STATE_DONE);
  if (void_ret) {
    ncc_buffer_puts(src, "return;\n");
  } else {
    ncc_buffer_printf(src, "return %s;\n", cache_name);
  }
}

static void emit_once_first_run_path(ncc_buffer_t *src, bool void_ret,
                                     const char *flag_name,
                                     const char *cache_name,
                                     const char *internal_name,
                                     const char *param_names) {
  ncc_buffer_printf(src, "case %d:\n", ONCE_GENERATED_STATE_UNCALLED);
  ncc_buffer_printf(src,
                    "if (!__atomic_compare_exchange_n(&%s, &expected, %d, 0, "
                    "__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {\n"
                    "goto wait_for_first_in;\n}\n",
                    flag_name, ONCE_GENERATED_STATE_RUNNING);
  if (void_ret) {
    ncc_buffer_printf(src, "%s(%s);\n", internal_name, param_names);
  } else {
    ncc_buffer_printf(src, "%s = %s(%s);\n", cache_name, internal_name,
                      param_names);
  }
  ncc_buffer_printf(src, "__atomic_store_n(&%s, %d, __ATOMIC_RELEASE);\n",
                    flag_name, ONCE_GENERATED_STATE_DONE);
}

static void emit_once_default_path_and_wrapper_close(ncc_buffer_t *src,
                                                     bool void_ret,
                                                     const char *cache_name) {
  if (void_ret) {
    ncc_buffer_puts(src, "return;\ndefault:\nreturn;\n}\n}\n");
  } else {
    ncc_buffer_printf(src, "return %s;\ndefault:\nreturn %s;\n}\n}\n",
                      cache_name, cache_name);
  }
}

static void emit_once_helper_definition(ncc_buffer_t *src, bool void_ret,
                                        const char *return_type,
                                        const char *params,
                                        const char *body,
                                        const char *internal_name) {
  ncc_buffer_printf(src, "static %s %s(%s)\n%s\n",
                    void_ret ? "void" : return_type, internal_name, params,
                    body);
}

static ncc_parse_tree_t *
parse_once_replacement(ncc_xform_ctx_t *ctx, bool void_ret, const char *fname,
                       const char *return_type, const char *quals,
                       const char *params, const char *param_names,
                       const char *body, const char *flag_name,
                       const char *cache_name, const char *internal_name) {
  ncc_buffer_t *src = ncc_buffer_empty();

  emit_once_declarations(src, void_ret, return_type, params, flag_name,
                         cache_name, internal_name);
  emit_once_wrapper_header(src, void_ret, fname, return_type, quals, params);
  emit_once_state_switch(src, flag_name);
  emit_once_done_path(src, void_ret, cache_name);
  emit_once_wait_path(src, void_ret, flag_name, cache_name);
  emit_once_first_run_path(src, void_ret, flag_name, cache_name,
                           internal_name, param_names);
  emit_once_default_path_and_wrapper_close(src, void_ret, cache_name);
  emit_once_helper_definition(src, void_ret, return_type, params, body,
                              internal_name);

  char *text = ncc_buffer_take(src);
  ncc_parse_tree_t *tree =
      ncc_xform_parse_source(ctx->grammar, "translation_unit", text,
                             "xform_once");
  ncc_free(text);
  return tree;
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

      char *fname = extract_func_name(declarator);
      if (!fname) {
        continue;
      }

      bool void_ret = ncc_xform_has_void_type(decl_specs);
      ncc_xform_data_t *xdata = ncc_xform_get_data(ctx);
      const char *pfx = xdata->once_prefix;

      // Build template arguments.
      char *base_type = ncc_xform_collect_base_type(decl_specs);
      char *return_modifier = collect_return_modifier_text(declarator);
      char *return_type = join_return_type(base_type, return_modifier);
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

      ncc_buffer_t *internal_buf = ncc_buffer_empty();
      ncc_buffer_puts(internal_buf, pfx);
      ncc_buffer_puts(internal_buf, fname);
      ncc_buffer_puts(internal_buf, "_once_body");
      char *internal_name = ncc_buffer_take(internal_buf);

      char *cache_name = nullptr;
      if (!void_ret) {
        ncc_buffer_t *cache_buf = ncc_buffer_empty();
        ncc_buffer_puts(cache_buf, pfx);
        ncc_buffer_puts(cache_buf, fname);
        ncc_buffer_puts(cache_buf, "_once_cache");
        cache_name = ncc_buffer_take(cache_buf);
      }

      ncc_parse_tree_t *new_tu =
          parse_once_replacement(ctx, void_ret, fname, return_type, quals,
                                 params, param_names, body, flag_name,
                                 cache_name, internal_name);

      if (!new_tu) {
        fprintf(stderr, "xform_once: replacement parse failed for '%s'\n",
                fname);
        ncc_free(fname);
        ncc_free(base_type);
        ncc_free(return_modifier);
        ncc_free(return_type);
        ncc_free(quals);
        ncc_free(params);
        ncc_free(param_names);
        ncc_free(body);
        ncc_free(flag_name);
        ncc_free(cache_name);
        ncc_free(internal_name);
        continue;
      }

      ncc_free(fname);
      ncc_free(base_type);
      ncc_free(return_modifier);
      ncc_free(return_type);
      ncc_free(quals);
      ncc_free(params);
      ncc_free(param_names);
      ncc_free(body);
      ncc_free(flag_name);
      ncc_free(cache_name);
      ncc_free(internal_name);

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
