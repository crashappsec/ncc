// xform_contracts.c — Transform: function contracts.
//
// Phase 2 lowering skeleton for `requires { ... }` / `ensures { ... }`.
// Contracted definitions become a public wrapper plus a static body helper.
// Top-level expression statements in contract blocks lower to debug-gated
// trap checks. Broader contract-block semantics are intentionally later work.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Text helpers copied locally from the _Once wrapper transform pattern.
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

static char *collect_param_list_text(ncc_parse_tree_t *declarator) {
  ncc_parse_tree_t *dd = ncc_xform_find_child_nt(declarator,
                                                 "direct_declarator");
  if (!dd) {
    dd = declarator;
  }

  ncc_parse_tree_t *ptl = ncc_xform_find_child_nt(dd,
                                                  "parameter_type_list");
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

static bool is_param_name_skip_word(const char *name) {
  return is_type_keyword(name) ||
         strcmp(name, "__const") == 0 ||
         strcmp(name, "__const__") == 0 ||
         strcmp(name, "__restrict") == 0 ||
         strcmp(name, "__restrict__") == 0 ||
         strcmp(name, "__volatile") == 0 ||
         strcmp(name, "__volatile__") == 0 ||
         strcmp(name, "_Nullable") == 0 ||
         strcmp(name, "_Nonnull") == 0 ||
         strcmp(name, "_Null_unspecified") == 0 ||
         strcmp(name, "__nullable") == 0 ||
         strcmp(name, "__nonnull") == 0 ||
         strcmp(name, "__null_unspecified") == 0;
}

static char *copy_param_name_from_text(const char *text) {
  size_t i = strlen(text);

  while (i > 0) {
    while (i > 0 && !is_ident_char(text[i - 1])) {
      i--;
    }

    size_t ident_end = i;
    while (i > 0 && is_ident_char(text[i - 1])) {
      i--;
    }

    if (ident_end <= i || !is_ident_start_char(text[i])) {
      continue;
    }

    size_t len = ident_end - i;
    char *name = ncc_alloc_size(1, len + 1);
    memcpy(name, text + i, len);
    name[len] = '\0';

    if (!is_param_name_skip_word(name)) {
      return name;
    }

    ncc_free(name);
  }

  return nullptr;
}

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} strvec_t;

static void strvec_push(strvec_t *v, char *s);

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

static bool text_contains(const char *haystack, const char *needle) {
  return haystack && needle && strstr(haystack, needle) != nullptr;
}

static char *copy_trimmed_range(const char *start, size_t len) {
  while (len > 0 && isspace((unsigned char)*start)) {
    start++;
    len--;
  }
  while (len > 0 && isspace((unsigned char)start[len - 1])) {
    len--;
  }

  char *result = ncc_alloc_size(1, len + 1);
  if (len > 0) {
    memcpy(result, start, len);
  }
  result[len] = '\0';
  return result;
}

static bool block_item_is_kargs_extract(ncc_parse_tree_t *item,
                                        ncc_string_t *text) {
  *text = ncc_xform_node_to_text(item);
  if (!text->data) {
    return false;
  }

  return text_contains(text->data, "maybe_unused") &&
         text_contains(text->data, "kargs ->");
}

static const char *skip_leading_maybe_unused(const char *text) {
  const char *p = skip_spaces(text);

  if (*p != '[') {
    return p;
  }

  const char *maybe = strstr(p, "maybe_unused");
  if (!maybe) {
    return p;
  }

  int close_brackets = 0;
  const char *q = maybe;
  while (*q && close_brackets < 2) {
    if (*q == ']') {
      close_brackets++;
    }
    q++;
  }

  return close_brackets == 2 ? skip_spaces(q) : p;
}

static const char *find_top_level_initializer_equal(const char *text) {
  int depth = 0;

  for (const char *p = text; *p; p++) {
    if (*p == '"' || *p == '\'') {
      p = skip_quoted(p);
      if (!*p) {
        return nullptr;
      }
      p--;
      continue;
    }
    if (*p == '(' || *p == '[' || *p == '{') {
      depth++;
      continue;
    }
    if ((*p == ')' || *p == ']' || *p == '}') && depth > 0) {
      depth--;
      continue;
    }
    if (*p == '=' && depth == 0) {
      return p;
    }
  }

  return nullptr;
}

static void collect_kargs_extract_decl_info(const char *item_text,
                                            strvec_t *helper_params,
                                            strvec_t *helper_names) {
  const char *decl_start = skip_leading_maybe_unused(item_text);
  const char *eq = find_top_level_initializer_equal(decl_start);
  if (!eq) {
    return;
  }

  char *decl_text =
      copy_trimmed_range(decl_start, (size_t)(eq - decl_start));
  char *clean = strip_declarator_attributes(decl_text);
  char *name = copy_param_name_from_text(clean);

  if (decl_text[0] && name) {
    strvec_push(helper_params, decl_text);
    strvec_push(helper_names, name);
  } else {
    ncc_free(decl_text);
    ncc_free(name);
  }

  ncc_free(clean);
}

static void collect_kargs_extracts(ncc_parse_tree_t *func_body,
                                   strvec_t *items,
                                   strvec_t *helper_params,
                                   strvec_t *helper_names) {
  ncc_parse_tree_t *compound =
      ncc_xform_find_child_nt(func_body, "compound_statement");
  ncc_parse_tree_t *bil =
      compound ? ncc_xform_find_child_nt(compound, "block_item_list")
               : nullptr;
  if (!bil) {
    return;
  }

  size_t nc = ncc_tree_num_children(bil);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *item = ncc_tree_child(bil, i);
    if (!item || ncc_tree_is_leaf(item)) {
      break;
    }

    ncc_string_t text = {};
    if (!block_item_is_kargs_extract(item, &text)) {
      ncc_free(text.data);
      break;
    }

    char *copy = ncc_alloc_size(1, text.u8_bytes + 1);
    memcpy(copy, text.data, text.u8_bytes);
    copy[text.u8_bytes] = '\0';
    collect_kargs_extract_decl_info(copy, helper_params, helper_names);
    ncc_free(text.data);
    strvec_push(items, copy);
  }
}

static char *collect_body_text_without_kargs_extracts(
    ncc_parse_tree_t *func_body) {
  ncc_parse_tree_t *compound =
      ncc_xform_find_child_nt(func_body, "compound_statement");
  ncc_parse_tree_t *bil =
      compound ? ncc_xform_find_child_nt(compound, "block_item_list")
               : nullptr;
  if (!bil) {
    return collect_body_text(func_body);
  }

  ncc_buffer_t *buf = ncc_buffer_empty();
  ncc_buffer_puts(buf, "{\n");

  bool skipping_extracts = true;
  size_t nc = ncc_tree_num_children(bil);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *item = ncc_tree_child(bil, i);
    if (!item || ncc_tree_is_leaf(item)) {
      continue;
    }

    ncc_string_t text = {};
    bool is_extract = block_item_is_kargs_extract(item, &text);
    if (skipping_extracts && is_extract) {
      ncc_free(text.data);
      continue;
    }
    skipping_extracts = false;

    if (!text.data) {
      text = ncc_xform_node_to_text(item);
    }
    if (text.data) {
      ncc_buffer_puts(buf, text.data);
      ncc_buffer_putc(buf, '\n');
      ncc_free(text.data);
    }
  }

  ncc_buffer_puts(buf, "}\n");
  return ncc_buffer_take(buf);
}

// ============================================================================
// Contract clause extraction
// ============================================================================

static void strvec_init(strvec_t *v) {
  v->items = nullptr;
  v->len = 0;
  v->cap = 0;
}

static void strvec_push(strvec_t *v, char *s) {
  if (v->len >= v->cap) {
    v->cap = v->cap ? v->cap * 2 : 4;
    v->items = ncc_realloc(v->items, v->cap * sizeof(char *));
  }
  v->items[v->len++] = s;
}

static bool strvec_contains(strvec_t *v, const char *s) {
  for (size_t i = 0; i < v->len; i++) {
    if (strcmp(v->items[i], s) == 0) {
      return true;
    }
  }
  return false;
}

static void strvec_free(strvec_t *v) {
  for (size_t i = 0; i < v->len; i++) {
    ncc_free(v->items[i]);
  }
  ncc_free(v->items);
  v->items = nullptr;
  v->len = 0;
  v->cap = 0;
}

static bool is_group_like_node(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }

  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.group_top || pn.group_item;
}

// ============================================================================
// Contract semantic analysis
// ============================================================================

typedef struct {
  strvec_t *scopes;
  size_t scope_len;
  size_t scope_cap;
  bool result_allowed;
} contract_semantic_ctx_t;

static void analyze_contract_semantics_node(contract_semantic_ctx_t *cctx,
                                            ncc_parse_tree_t *node);

static void contract_semantic_error(ncc_parse_tree_t *node,
                                    const char *message) {
  uint32_t line, col;
  ncc_xform_first_leaf_pos(node, &line, &col);
  fprintf(stderr, "ncc: error: %s (line %u, col %u)\n",
          message, line, col);
  exit(1);
}

static ncc_parse_tree_t *direct_child_nt(ncc_parse_tree_t *node,
                                         const char *name) {
  if (!node || ncc_tree_is_leaf(node)) {
    return nullptr;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (child && !ncc_tree_is_leaf(child) &&
        ncc_xform_nt_name_is(child, name)) {
      return child;
    }
  }

  return nullptr;
}

static bool node_has_direct_leaf(ncc_parse_tree_t *node, const char *text) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (child && ncc_tree_is_leaf(child) &&
        ncc_xform_leaf_text_eq(child, text)) {
      return true;
    }
  }

  return false;
}

static bool node_first_direct_leaf_is(ncc_parse_tree_t *node,
                                      const char *text) {
  if (!node || ncc_tree_is_leaf(node) ||
      ncc_tree_num_children(node) == 0) {
    return false;
  }

  ncc_parse_tree_t *child = ncc_tree_child(node, 0);
  return child && ncc_tree_is_leaf(child) &&
         ncc_xform_leaf_text_eq(child, text);
}

static const char *identifier_name(ncc_parse_tree_t *node) {
  if (!node || !ncc_xform_nt_name_is(node, "identifier")) {
    return nullptr;
  }

  return ncc_xform_get_first_leaf_text(node);
}

static bool subtree_has_identifier_named(ncc_parse_tree_t *node,
                                         const char *name) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  if (ncc_xform_nt_name_is(node, "_member_name")) {
    return false;
  }
  if (ncc_xform_nt_name_is(node, "identifier")) {
    const char *id = identifier_name(node);
    return id && strcmp(id, name) == 0;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (subtree_has_identifier_named(ncc_tree_child(node, i), name)) {
      return true;
    }
  }
  return false;
}

static const char *plain_identifier_target_name(ncc_parse_tree_t *node) {
  while (node && !ncc_tree_is_leaf(node) &&
         !ncc_xform_nt_name_is(node, "identifier")) {
    if (ncc_xform_nt_name_is(node, "primary_expression") &&
        node_first_direct_leaf_is(node, "(")) {
      ncc_parse_tree_t *expr = direct_child_nt(node, "expression");
      if (!expr) {
        return nullptr;
      }
      node = expr;
      continue;
    }
    if (ncc_tree_num_children(node) != 1) {
      return nullptr;
    }
    ncc_parse_tree_t *child = ncc_tree_child(node, 0);
    if (!child || ncc_tree_is_leaf(child)) {
      return nullptr;
    }
    node = child;
  }

  return identifier_name(node);
}

static bool postfix_expression_is_call(ncc_parse_tree_t *node) {
  if (!ncc_xform_nt_name_is(node, "postfix_expression") ||
      !node_has_direct_leaf(node, "(")) {
    return false;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (!child || ncc_tree_is_leaf(child)) {
      continue;
    }
    return ncc_xform_nt_name_is(child, "postfix_expression");
  }

  return false;
}

static bool postfix_expression_is_inc_dec(ncc_parse_tree_t *node) {
  return ncc_xform_nt_name_is(node, "postfix_expression") &&
         (node_has_direct_leaf(node, "++") ||
          node_has_direct_leaf(node, "--"));
}

static bool unary_expression_is_prefix_inc_dec(ncc_parse_tree_t *node) {
  if (!ncc_xform_nt_name_is(node, "unary_expression")) {
    return false;
  }

  if (ncc_tree_num_children(node) < 2) {
    return false;
  }

  ncc_parse_tree_t *first = ncc_tree_child(node, 0);
  return first && ncc_tree_is_leaf(first) &&
         (ncc_xform_leaf_text_eq(first, "++") ||
          ncc_xform_leaf_text_eq(first, "--"));
}

static ncc_parse_tree_t *assignment_lhs(ncc_parse_tree_t *node) {
  if (!ncc_xform_nt_name_is(node, "assignment_expression")) {
    return nullptr;
  }

  ncc_parse_tree_t *op = direct_child_nt(node, "assignment_operator");
  if (!op) {
    return nullptr;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (child == op) {
      break;
    }
    if (child && !ncc_tree_is_leaf(child)) {
      return child;
    }
  }

  return nullptr;
}

static ncc_parse_tree_t *assignment_rhs(ncc_parse_tree_t *node) {
  ncc_parse_tree_t *op = direct_child_nt(node, "assignment_operator");
  if (!op) {
    return nullptr;
  }

  bool seen_op = false;
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (child == op) {
      seen_op = true;
      continue;
    }
    if (seen_op && child && !ncc_tree_is_leaf(child)) {
      return child;
    }
  }

  return nullptr;
}

static void contract_scope_push(contract_semantic_ctx_t *cctx) {
  if (cctx->scope_len >= cctx->scope_cap) {
    cctx->scope_cap = cctx->scope_cap ? cctx->scope_cap * 2 : 4;
    cctx->scopes =
        ncc_realloc(cctx->scopes, cctx->scope_cap * sizeof(strvec_t));
  }

  strvec_init(&cctx->scopes[cctx->scope_len++]);
}

static void contract_scope_pop(contract_semantic_ctx_t *cctx) {
  if (cctx->scope_len == 0) {
    return;
  }

  cctx->scope_len--;
  strvec_free(&cctx->scopes[cctx->scope_len]);
}

static void contract_scope_free(contract_semantic_ctx_t *cctx) {
  while (cctx->scope_len > 0) {
    contract_scope_pop(cctx);
  }
  ncc_free(cctx->scopes);
  cctx->scopes = nullptr;
  cctx->scope_cap = 0;
}

static bool contract_name_is_local(contract_semantic_ctx_t *cctx,
                                   const char *name) {
  for (size_t i = cctx->scope_len; i > 0; i--) {
    if (strvec_contains(&cctx->scopes[i - 1], name)) {
      return true;
    }
  }

  return false;
}

static void check_mutation_target(contract_semantic_ctx_t *cctx,
                                  ncc_parse_tree_t *target) {
  if (!cctx->result_allowed &&
      subtree_has_identifier_named(target, "result")) {
    contract_semantic_error(target,
                            "result is only available in non-void ensures");
  }

  const char *name = plain_identifier_target_name(target);
  if (!name || !contract_name_is_local(cctx, name)) {
    contract_semantic_error(target,
                            "contract may mutate only contract-local "
                            "variables");
  }
}

static void add_declarator_local(contract_semantic_ctx_t *cctx,
                                 ncc_parse_tree_t *declarator) {
  ncc_string_t text = ncc_xform_node_to_text(declarator);
  if (!text.data) {
    return;
  }

  char *clean = strip_declarator_attributes(text.data);
  char *name = copy_param_name_from_text(clean);
  ncc_free(text.data);
  ncc_free(clean);

  if (!name) {
    return;
  }

  if (strcmp(name, "result") == 0) {
    ncc_free(name);
    contract_semantic_error(declarator,
                            "result may not be declared in function "
                            "contracts");
  }

  if (cctx->scope_len == 0) {
    ncc_free(name);
    return;
  }

  strvec_t *scope = &cctx->scopes[cctx->scope_len - 1];
  if (!strvec_contains(scope, name)) {
    strvec_push(scope, name);
  } else {
    ncc_free(name);
  }
}

static void analyze_init_declarator_semantics(contract_semantic_ctx_t *cctx,
                                              ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(node,
                                                         "declarator");
  if (declarator) {
    add_declarator_local(cctx, declarator);
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (child && !ncc_tree_is_leaf(child)) {
      analyze_contract_semantics_node(cctx, child);
    }
  }
}

static void analyze_init_declarator_list_semantics(
    contract_semantic_ctx_t *cctx, ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (!child || ncc_tree_is_leaf(child)) {
      continue;
    }
    if (ncc_xform_nt_name_is(child, "init_declarator")) {
      analyze_init_declarator_semantics(cctx, child);
    } else {
      analyze_contract_semantics_node(cctx, child);
    }
  }
}

static void analyze_declaration_semantics(contract_semantic_ctx_t *cctx,
                                          ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (!child || ncc_tree_is_leaf(child)) {
      continue;
    }
    if (ncc_xform_nt_name_is(child, "init_declarator_list")) {
      analyze_init_declarator_list_semantics(cctx, child);
      continue;
    }
    if (ncc_xform_nt_name_is(child, "declarator")) {
      add_declarator_local(cctx, child);
    }
    analyze_contract_semantics_node(cctx, child);
  }
}

static void analyze_contract_semantics_node(contract_semantic_ctx_t *cctx,
                                            ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  if (ncc_xform_nt_name_is(node, "struct_or_union_specifier") ||
      ncc_xform_nt_name_is(node, "enum_specifier") ||
      ncc_xform_nt_name_is(node, "_member_name")) {
    return;
  }

  if (ncc_xform_nt_name_is(node, "compound_statement") ||
      ncc_xform_nt_name_is(node, "selection_statement") ||
      ncc_xform_nt_name_is(node, "iteration_statement")) {
    contract_scope_push(cctx);
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
      analyze_contract_semantics_node(cctx, ncc_tree_child(node, i));
    }
    contract_scope_pop(cctx);
    return;
  }

  if (ncc_xform_nt_name_is(node, "identifier")) {
    const char *name = identifier_name(node);
    if (name && strcmp(name, "result") == 0 && !cctx->result_allowed) {
      contract_semantic_error(node,
                              "result is only available in non-void "
                              "ensures");
    }
    return;
  }

  if (ncc_xform_nt_name_is(node, "init_declarator_list")) {
    analyze_init_declarator_list_semantics(cctx, node);
    return;
  }

  if (ncc_xform_nt_name_is(node, "init_declarator")) {
    analyze_init_declarator_semantics(cctx, node);
    return;
  }

  if (ncc_xform_nt_name_is(node, "declaration") ||
      ncc_xform_nt_name_is(node, "simple_declaration")) {
    analyze_declaration_semantics(cctx, node);
    return;
  }

  if (ncc_xform_nt_name_is(node, "jump_statement")) {
    const char *first = ncc_xform_get_first_leaf_text(node);
    if (first && strcmp(first, "return") == 0) {
      contract_semantic_error(node,
                              "return is not allowed in function contracts");
    }
    if (first && strcmp(first, "goto") == 0) {
      contract_semantic_error(node,
                              "goto is not allowed in function contracts");
    }
  }

  if (postfix_expression_is_call(node)) {
    contract_semantic_error(node,
                            "function calls are not allowed in function "
                            "contracts");
  }

  ncc_parse_tree_t *lhs = assignment_lhs(node);
  if (lhs) {
    check_mutation_target(cctx, lhs);
    analyze_contract_semantics_node(cctx, assignment_rhs(node));
    return;
  }

  if (postfix_expression_is_inc_dec(node)) {
    ncc_parse_tree_t *target = ncc_tree_num_children(node) > 0
                                   ? ncc_tree_child(node, 0)
                                   : nullptr;
    check_mutation_target(cctx, target);
    return;
  }

  if (unary_expression_is_prefix_inc_dec(node)) {
    ncc_parse_tree_t *target = ncc_tree_num_children(node) > 1
                                   ? ncc_tree_child(node, 1)
                                   : nullptr;
    check_mutation_target(cctx, target);
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    analyze_contract_semantics_node(cctx, ncc_tree_child(node, i));
  }
}

static void analyze_contract_clause_semantics(ncc_parse_tree_t *clause,
                                              bool result_allowed) {
  if (!clause) {
    return;
  }

  contract_semantic_ctx_t cctx = {};
  cctx.result_allowed = result_allowed;
  analyze_contract_semantics_node(&cctx, clause);
  contract_scope_free(&cctx);
}

static ncc_parse_tree_t *find_top_level_expr_stmt(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return nullptr;
  }
  if (ncc_xform_nt_name_is(node, "expression_statement")) {
    return node;
  }
  if (!ncc_xform_nt_name_is(node, "block_item") &&
      !ncc_xform_nt_name_is(node, "unlabeled_statement") &&
      !is_group_like_node(node)) {
    return nullptr;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *expr =
        find_top_level_expr_stmt(ncc_tree_child(node, i));
    if (expr) {
      return expr;
    }
  }
  return nullptr;
}

static void push_clause_block_item_text(ncc_parse_tree_t *item,
                                        strvec_t *items) {
  ncc_parse_tree_t *expr_stmt = find_top_level_expr_stmt(item);
  if (expr_stmt) {
    ncc_parse_tree_t *expr =
        ncc_xform_find_child_nt(expr_stmt, "expression");
    if (expr) {
      ncc_string_t text = ncc_xform_node_to_text(expr);
      if (text.data) {
        ncc_buffer_t *check = ncc_buffer_empty();
        ncc_buffer_printf(check,
                          "if (!(%s)) { __builtin_trap(); }",
                          text.data);
        ncc_free(text.data);
        strvec_push(items, ncc_buffer_take(check));
        return;
      }
    }
  }

  ncc_string_t text = ncc_xform_node_to_text(item);
  if (!text.data) {
    return;
  }

  char *copy = ncc_alloc_size(1, text.u8_bytes + 1);
  memcpy(copy, text.data, text.u8_bytes);
  copy[text.u8_bytes] = '\0';
  ncc_free(text.data);
  strvec_push(items, copy);
}

static void collect_clause_block_items(ncc_parse_tree_t *node,
                                       strvec_t *items) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  if (ncc_xform_nt_name_is(node, "block_item")) {
    push_clause_block_item_text(node, items);
    return;
  }

  if (!ncc_xform_nt_name_is(node, "block_item_list") &&
      !is_group_like_node(node)) {
    return;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    collect_clause_block_items(ncc_tree_child(node, i), items);
  }
}

static void collect_clause_items(ncc_parse_tree_t *clause, strvec_t *items) {
  ncc_parse_tree_t *compound =
      ncc_xform_find_child_nt(clause, "compound_statement");
  ncc_parse_tree_t *block_items =
      ncc_xform_find_child_nt(compound, "block_item_list");
  if (!block_items) {
    return;
  }

  collect_clause_block_items(block_items, items);
}

// ============================================================================
// Generated-C rendering and debug directive wrapping
// ============================================================================

static bool text_is_void_param_list(const char *params) {
  const char *start = skip_spaces(params);
  size_t len = strlen(start);
  while (len > 0 && isspace((unsigned char)start[len - 1])) {
    len--;
  }

  return len == 4 && memcmp(start, "void", 4) == 0;
}

static void append_joined_text(ncc_buffer_t *buf, bool *first,
                               const char *text) {
  if (!*first) {
    ncc_buffer_puts(buf, ", ");
  }
  ncc_buffer_puts(buf, text);
  *first = false;
}

static void build_helper_signature_parts(const char *params,
                                         strvec_t *karg_helper_params,
                                         strvec_t *karg_helper_names,
                                         char **out_params,
                                         char **out_names) {
  ncc_buffer_t *param_buf = ncc_buffer_empty();
  ncc_buffer_t *name_buf = ncc_buffer_empty();
  bool first_param = true;
  bool first_name = true;

  if (!text_is_void_param_list(params)) {
    int depth = 0;
    size_t start = 0;
    size_t len = strlen(params);

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
        char *part = copy_trimmed_range(params + start, i - start);
        char *clean = strip_declarator_attributes(part);
        char *name = copy_param_name_from_text(clean);

        if (name && strcmp(name, "kargs") != 0) {
          append_joined_text(param_buf, &first_param, part);
          append_joined_text(name_buf, &first_name, name);
        }

        ncc_free(part);
        ncc_free(clean);
        ncc_free(name);
        start = i + 1;
      }
    }
  }

  for (size_t i = 0; i < karg_helper_params->len; i++) {
    append_joined_text(param_buf, &first_param, karg_helper_params->items[i]);
    append_joined_text(name_buf, &first_name, karg_helper_names->items[i]);
  }

  if (first_param) {
    ncc_buffer_puts(param_buf, "void");
  }

  *out_params = ncc_buffer_take(param_buf);
  *out_names = ncc_buffer_take(name_buf);
}

static void emit_contract_block(ncc_buffer_t *src, strvec_t *items) {
  ncc_buffer_puts(src, "{\n");
  for (size_t i = 0; i < items->len; i++) {
    ncc_buffer_puts(src, items->items[i]);
    ncc_buffer_putc(src, '\n');
  }
  ncc_buffer_puts(src, "}\n");
}

static void emit_contract_wrapper(ncc_buffer_t *src, bool void_ret,
                                  const char *fname,
                                  const char *return_type,
                                  const char *quals,
                                  const char *params,
                                  const char *helper_param_names,
                                  const char *helper_name,
                                  strvec_t *kargs_extracts,
                                  bool has_requires,
                                  strvec_t *requires_items,
                                  bool has_ensures,
                                  strvec_t *ensures_items) {
  if (quals && quals[0]) {
    ncc_buffer_printf(src, "%s ", quals);
  }
  ncc_buffer_printf(src, "%s %s(%s) {\n",
                    void_ret ? "void" : return_type, fname, params);

  for (size_t i = 0; i < kargs_extracts->len; i++) {
    ncc_buffer_puts(src, kargs_extracts->items[i]);
    ncc_buffer_putc(src, '\n');
  }

  if (has_requires) {
    emit_contract_block(src, requires_items);
  }

  if (void_ret) {
    ncc_buffer_printf(src, "%s(%s);\n", helper_name, helper_param_names);
  } else {
    ncc_buffer_printf(src, "%s result = %s(%s);\n",
                      return_type, helper_name, helper_param_names);
  }

  if (has_ensures) {
    emit_contract_block(src, ensures_items);
  }

  if (void_ret) {
    ncc_buffer_puts(src, "return;\n}\n");
  } else {
    ncc_buffer_puts(src, "return result;\n}\n");
  }
}

static ncc_parse_tree_t *raw_token(const char *text) {
  return ncc_xform_make_token_node(0, text, 0, 0);
}

static ncc_parse_tree_t *debug_guard_group(ncc_parse_tree_t *item) {
  ncc_nt_node_t gpn = {};
  gpn.name = ncc_string_from_cstr("$$group_contract_debug");
  gpn.id = (1 << 28) + 17;
  gpn.group_top = true;

  ncc_parse_tree_t *group =
      ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, gpn);
  ncc_tree_add_child(group, raw_token("\n#ifndef NDEBUG\n"));
  ncc_tree_add_child(group, item);
  ncc_tree_add_child(group, raw_token("\n#endif\n"));
  ncc_xform_set_parent_pointers(group);
  return group;
}

static bool is_group_node(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.group_top || pn.group_item;
}

static bool has_top_level_compound_statement(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(node, i);
    if (!child || ncc_tree_is_leaf(child)) {
      continue;
    }
    if (ncc_xform_nt_name_is(child, "compound_statement")) {
      return true;
    }
    if (ncc_xform_nt_name_is(child, "unlabeled_statement") ||
        ncc_xform_nt_name_is(child, "primary_block") ||
        is_group_node(child)) {
      if (has_top_level_compound_statement(child)) {
        return true;
      }
    }
  }

  return false;
}

static bool subtree_contains_leaf(ncc_parse_tree_t *node, const char *text) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, text);
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (subtree_contains_leaf(ncc_tree_child(node, i), text)) {
      return true;
    }
  }
  return false;
}

static void wrap_top_level_contract_blocks(ncc_parse_tree_t *parent) {
  if (!parent || ncc_tree_is_leaf(parent)) {
    return;
  }

  size_t nc = ncc_tree_num_children(parent);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *child = ncc_tree_child(parent, i);
    if (!child || ncc_tree_is_leaf(child)) {
      continue;
    }
    if (ncc_xform_nt_name_is(child, "block_item")) {
      if (has_top_level_compound_statement(child)) {
        ncc_tree_set_child(parent, i, debug_guard_group(child));
      }
      continue;
    }
    if (is_group_node(child)) {
      wrap_top_level_contract_blocks(child);
    }
  }
}

static void wrap_wrapper_debug_checks(ncc_parse_tree_t *tree,
                                      const char *wrapper_name) {
  if (!tree || ncc_tree_is_leaf(tree)) {
    return;
  }

  if (ncc_xform_nt_name_is(tree, "function_definition")) {
    ncc_parse_tree_t *declarator =
        ncc_xform_find_child_nt(tree, "declarator");
    char *name = declarator ? extract_func_name(declarator) : nullptr;
    bool is_wrapper = name && strcmp(name, wrapper_name) == 0;
    ncc_free(name);

    if (is_wrapper) {
      ncc_parse_tree_t *body = ncc_xform_find_child_nt(tree,
                                                       "function_body");
      ncc_parse_tree_t *compound =
          ncc_xform_find_child_nt(body, "compound_statement");
      ncc_parse_tree_t *items =
          ncc_xform_find_child_nt(compound, "block_item_list");
      wrap_top_level_contract_blocks(items);
      return;
    }
  }

  size_t nc = ncc_tree_num_children(tree);
  for (size_t i = 0; i < nc; i++) {
    wrap_wrapper_debug_checks(ncc_tree_child(tree, i), wrapper_name);
  }
}

static ncc_parse_tree_t *
parse_contract_replacement(ncc_xform_ctx_t *ctx, bool void_ret,
                           const char *fname, const char *return_type,
                           const char *quals, const char *params,
                           const char *helper_params,
                           const char *helper_param_names,
                           const char *body, const char *helper_name,
                           strvec_t *kargs_extracts,
                           bool has_requires,
                           strvec_t *requires_items,
                           bool has_ensures,
                           strvec_t *ensures_items) {
  ncc_buffer_t *src = ncc_buffer_empty();

  ncc_buffer_printf(src, "static %s %s(%s);\n",
                    void_ret ? "void" : return_type, helper_name,
                    helper_params);
  emit_contract_wrapper(src, void_ret, fname, return_type, quals, params,
                        helper_param_names, helper_name, kargs_extracts,
                        has_requires, requires_items, has_ensures,
                        ensures_items);
  ncc_buffer_printf(src, "static %s %s(%s)\n%s\n",
                    void_ret ? "void" : return_type, helper_name,
                    helper_params, body);

  char *text = ncc_buffer_take(src);
  ncc_parse_tree_t *tree =
      ncc_xform_parse_source(ctx->grammar, "translation_unit", text,
                             "xform_contracts");
  ncc_free(text);
  if (tree) {
    wrap_wrapper_debug_checks(tree, fname);
  }
  return tree;
}

static ncc_parse_tree_t *
unwrap_extension_function_definition(ncc_parse_tree_t *func_def) {
  while (func_def && ncc_xform_nt_name_is(func_def, "function_definition") &&
         ncc_tree_num_children(func_def) == 2) {
    ncc_parse_tree_t *first = ncc_tree_child(func_def, 0);
    ncc_parse_tree_t *second = ncc_tree_child(func_def, 1);
    if (!first || !ncc_tree_is_leaf(first) ||
        !ncc_xform_leaf_text_eq(first, "__extension__") ||
        !second || ncc_tree_is_leaf(second) ||
        !ncc_xform_nt_name_is(second, "function_definition")) {
      break;
    }
    func_def = second;
  }

  return func_def;
}

// ============================================================================
// Flattening infrastructure
// ============================================================================

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
// Main post-order transform on translation_unit
// ============================================================================

static ncc_parse_tree_t *
xform_contracts_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu) {
  ptrvec_t flat;
  ptrvec_init(&flat, 256);
  flatten_tu(tu, &flat);

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
    if (!inner || !ncc_xform_nt_name_is(inner, "function_definition")) {
      continue;
    }
    inner = unwrap_extension_function_definition(inner);

    ncc_parse_tree_t *contracts =
        ncc_xform_find_child_nt(inner, "function_contract_clauses");
    if (!contracts) {
      continue;
    }

    if (ncc_xform_find_child_nt(inner, "keyword_clause")) {
      uint32_t line, col;
      ncc_xform_first_leaf_pos(inner, &line, &col);
      fprintf(stderr,
              "ncc: error: function contracts must run after _kargs "
              "lowering (line %u, col %u)\n",
              line, col);
      exit(1);
    }

    ncc_parse_tree_t *decl_specs =
        ncc_xform_find_child_nt(inner, "declaration_specifiers");
    ncc_parse_tree_t *declarator =
        ncc_xform_find_child_nt(inner, "declarator");
    ncc_parse_tree_t *func_body =
        ncc_xform_find_child_nt(inner, "function_body");
    if (!decl_specs || !declarator || !func_body) {
      continue;
    }

    if (subtree_contains_leaf(decl_specs, "_Once")) {
      uint32_t line, col;
      ncc_xform_first_leaf_pos(inner, &line, &col);
      fprintf(stderr,
              "ncc: error: '_Once' functions with contracts are not "
              "supported yet (line %u, col %u)\n",
              line, col);
      exit(1);
    }

    char *fname = extract_func_name(declarator);
    if (!fname) {
      continue;
    }

    strvec_t requires_items;
    strvec_t ensures_items;
    strvec_t kargs_extracts;
    strvec_t karg_helper_params;
    strvec_t karg_helper_names;
    strvec_init(&requires_items);
    strvec_init(&ensures_items);
    strvec_init(&kargs_extracts);
    strvec_init(&karg_helper_params);
    strvec_init(&karg_helper_names);

    ncc_parse_tree_t *requires_clause =
        ncc_xform_find_child_nt(contracts, "requires_clause");
    ncc_parse_tree_t *ensures_clause =
        ncc_xform_find_child_nt(contracts, "ensures_clause");

    char *base_type = ncc_xform_collect_base_type(decl_specs);
    char *return_modifier = collect_return_modifier_text(declarator);
    char *return_type = join_return_type(base_type, return_modifier);
    bool void_ret =
        ncc_xform_has_void_type(decl_specs) && return_modifier[0] == '\0';

    analyze_contract_clause_semantics(requires_clause, false);
    analyze_contract_clause_semantics(ensures_clause, !void_ret);

    if (requires_clause) {
      collect_clause_items(requires_clause, &requires_items);
    }
    if (ensures_clause) {
      collect_clause_items(ensures_clause, &ensures_items);
    }

    char *quals = ncc_xform_collect_qualifiers(decl_specs, nullptr);
    char *params = collect_param_list_text(declarator);
    collect_kargs_extracts(func_body, &kargs_extracts, &karg_helper_params,
                           &karg_helper_names);
    char *helper_params = nullptr;
    char *helper_param_names = nullptr;
    build_helper_signature_parts(params, &karg_helper_params,
                                 &karg_helper_names, &helper_params,
                                 &helper_param_names);
    char *body = kargs_extracts.len > 0
                     ? collect_body_text_without_kargs_extracts(func_body)
                     : collect_body_text(func_body);

    ncc_buffer_t *helper_buf = ncc_buffer_empty();
    ncc_buffer_puts(helper_buf, "__ncc_cw_");
    ncc_buffer_puts(helper_buf, fname);
    ncc_buffer_puts(helper_buf, "_body");
    char *helper_name = ncc_buffer_take(helper_buf);

    ncc_parse_tree_t *new_tu =
        parse_contract_replacement(ctx, void_ret, fname, return_type, quals,
                                   params, helper_params,
                                   helper_param_names, body, helper_name,
                                   &kargs_extracts, requires_clause != nullptr,
                                   &requires_items, ensures_clause != nullptr,
                                   &ensures_items);

    if (!new_tu) {
      fprintf(stderr,
              "xform_contracts: replacement parse failed for '%s'\n",
              fname);
      ncc_free(fname);
      strvec_free(&requires_items);
      strvec_free(&ensures_items);
      strvec_free(&kargs_extracts);
      strvec_free(&karg_helper_params);
      strvec_free(&karg_helper_names);
      ncc_free(base_type);
      ncc_free(return_modifier);
      ncc_free(return_type);
      ncc_free(quals);
      ncc_free(params);
      ncc_free(helper_params);
      ncc_free(helper_param_names);
      ncc_free(body);
      ncc_free(helper_name);
      continue;
    }

    ncc_free(fname);
    strvec_free(&requires_items);
    strvec_free(&ensures_items);
    strvec_free(&kargs_extracts);
    strvec_free(&karg_helper_params);
    strvec_free(&karg_helper_names);
    ncc_free(base_type);
    ncc_free(return_modifier);
    ncc_free(return_type);
    ncc_free(quals);
    ncc_free(params);
    ncc_free(helper_params);
    ncc_free(helper_param_names);
    ncc_free(body);
    ncc_free(helper_name);

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
  }

  if (!changed) {
    ncc_free(flat.items);
    return nullptr;
  }

  ncc_nt_node_t gpn = {};
  gpn.name = ncc_string_from_cstr("$$group_contracts");
  gpn.id = (1 << 28) + 18;
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
  ncc_xform_set_parent_pointers(tu);

  return tu;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_contracts_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "translation_unit", xform_contracts_tu,
                     "contracts");
}
