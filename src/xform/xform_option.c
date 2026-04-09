// xform_option.c — Transform: first-class _option(T) type.
//
// For pointer types, _option(T*) = T* (nullptr is none).
// For value types, _option(T) = struct { bool has_value; T value; }
// with a unique name via typeid-style mangling.
//
// Also transforms: _some(T, val), _none(T), _is_some(x), _is_none(x),
// _unwrap(x).
//
// Two registrations:
//   - "synthetic_identifier" (post-order) for _option(T)
//   - "postfix_expression"  (post-order) for
//   _some/_none/_is_some/_is_none/_unwrap

#include "lib/alloc.h"
#include "lib/dict.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Access to shared user_data dictionaries
// ============================================================================

static ncc_dict_t *get_option_meta(ncc_xform_ctx_t *ctx) {
  ncc_xform_data_t *d = ncc_xform_get_data(ctx);
  return &d->option_meta;
}

static ncc_dict_t *get_option_decls(ncc_xform_ctx_t *ctx) {
  ncc_xform_data_t *d = ncc_xform_get_data(ctx);
  return &d->option_decls;
}

// ============================================================================
// Helpers
// ============================================================================

// Check whether a type string represents a pointer type.
static bool type_string_is_pointer(const char *type_str) {
  return type_str && strchr(type_str, '*') != nullptr;
}

// Template parsing helper.
static ncc_parse_tree_t *parse_template(ncc_grammar_t *g, const char *nt_name,
                                        const char *src) {
  return ncc_xform_parse_source(g, nt_name, src, "xform_option");
}

// Walk up to find the variable name being declared.
// Looks for ancestor "declaration" → "init_declarator_list" →
// "init_declarator" → "declarator" → leaf identifier.
static const char *find_declared_var_name(ncc_parse_tree_t *node) {
  ncc_parse_tree_t *decl = ncc_xform_find_ancestor(node, "declaration");
  if (!decl) {
    return nullptr;
  }

  ncc_parse_tree_t *init_list =
      ncc_xform_find_child_nt(decl, "init_declarator_list");
  if (!init_list) {
    return nullptr;
  }

  ncc_parse_tree_t *init_decl =
      ncc_xform_find_child_nt(init_list, "init_declarator");
  if (!init_decl) {
    init_decl = init_list;
  }

  ncc_parse_tree_t *declarator =
      ncc_xform_find_child_nt(init_decl, "declarator");
  if (!declarator) {
    return nullptr;
  }

  ncc_parse_tree_t *dd =
      ncc_xform_find_child_nt(declarator, "direct_declarator");
  if (!dd) {
    dd = declarator;
  }

  ncc_parse_tree_t *ident = ncc_xform_find_child_nt(dd, "identifier");
  if (!ident) {
    // direct_declarator might contain identifier directly or
    // through provided_identifier.
    ident = ncc_xform_find_child_nt(dd, "provided_identifier");
  }
  if (ident) {
    // Walk down to find the first leaf token.
    ncc_parse_tree_t *cur = ident;
    while (cur && !ncc_tree_is_leaf(cur)) {
      size_t cnc = ncc_tree_num_children(cur);
      ncc_parse_tree_t *next = nullptr;
      for (size_t i = 0; i < cnc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(cur, i);
        if (c) {
          if (ncc_tree_is_leaf(c)) {
            return ncc_xform_leaf_text(c);
          }
          if (!next) {
            next = c;
          }
        }
      }
      cur = next;
    }
    if (cur && ncc_tree_is_leaf(cur)) {
      return ncc_xform_leaf_text(cur);
    }
  }

  // Fallback: first non-punctuation leaf child.
  size_t nc = ncc_tree_num_children(dd);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(dd, i);
    if (c && ncc_tree_is_leaf(c)) {
      const char *text = ncc_xform_leaf_text(c);
      if (text && text[0] != '(' && text[0] != ')') {
        return text;
      }
    }
  }

  return nullptr;
}

// Build "option, T" type string for mangling (produces unique struct name).
static char *option_type_string(const char *type_str) {
  size_t len = strlen("option, ") + strlen(type_str) + 1;
  char *buf = ncc_alloc_size(1, len);
  snprintf(buf, len, "option, %s", type_str);
  return buf;
}

// Find the last leaf token in a subtree — uses shared helper.
static ncc_token_info_t *find_last_leaf_token(ncc_parse_tree_t *node) {
  return ncc_xform_find_last_leaf_token(node);
}

// Emit a struct declaration at the translation_unit level.
// Inserts: struct <mangled> { bool has_value; <type_str> value; };
// as the first child of the root translation_unit node.
static void emit_struct_decl(ncc_xform_ctx_t *ctx, const char *mangled,
                             const char *type_str) {
  ncc_dict_t *decls = get_option_decls(ctx);
  if (ncc_dict_contains(decls, (void *)mangled)) {
    return;
  }
  ncc_dict_put(decls, strdup(mangled), (void *)(uintptr_t)1);

  char src[1024];
  snprintf(src, sizeof(src), "struct %s { bool has_value; %s value; };",
           mangled, type_str);

  ncc_parse_tree_t *decl_tree =
      parse_template(ctx->grammar, "external_declaration", src);
  if (!decl_tree) {
    fprintf(stderr, "ncc: error: failed to emit option struct '%s'\n", mangled);
    exit(1);
  }

  // Add trailing newline trivia to the last token of the struct decl
  // so it doesn't run into the next line's #line directive.
  ncc_token_info_t *last_tok = find_last_leaf_token(decl_tree);
  if (last_tok) {
    ncc_trivia_t *nl = ncc_alloc(ncc_trivia_t);
    nl->text = ncc_string_from_cstr("\n");
    nl->next = last_tok->trailing_trivia;
    last_tok->trailing_trivia = nl;
  }

  ncc_xform_insert_child(ctx->root, 0, decl_tree);
}

// ============================================================================
// Transform: _option(T) on synthetic_identifier
// ============================================================================

static ncc_parse_tree_t *xform_option_type(ncc_xform_ctx_t *ctx,
                                           ncc_parse_tree_t *node) {
  size_t nc = ncc_tree_num_children(node);
  if (nc < 4) {
    return nullptr;
  }

  // Check first child is the _named_id_kw_option NT containing "_option".
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

  if (!kw_text || strcmp(kw_text, "_option") != 0) {
    return nullptr;
  }

  // Extract type string from typeid_atom and optional typeid_continuation.
  ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
  ncc_parse_tree_t *cont = ncc_xform_find_child_nt(node, "typeid_continuation");
  if (!atom) {
    return nullptr;
  }

  ncc_string_t type_str = ncc_xform_extract_type_string(ctx, atom, cont);
  bool is_pointer = type_string_is_pointer(type_str.data);

  uint32_t line, col;
  ncc_xform_first_leaf_pos(node, &line, &col);

  // Record in metadata dict for _is_some/_unwrap etc.
  const char *var_name = find_declared_var_name(node);
  if (var_name) {
    ncc_dict_t *meta = get_option_meta(ctx);
    ncc_dict_put(meta, strdup(var_name), (void *)(uintptr_t)is_pointer);
  }

  ncc_parse_tree_t *replacement;

  if (is_pointer) {
    // Pointer type: _option(T*) = T* (same ABI, nullptr = none).
    replacement =
        ncc_xform_make_token_node(NCC_TOK_IDENTIFIER, type_str.data, line, col);
  } else {
    // Value type: _option(T) = struct <mangled>.
    char *opt_str = option_type_string(type_str.data);
    ncc_string_t mangled = ncc_type_mangle(opt_str);
    ncc_free(opt_str);

    emit_struct_decl(ctx, mangled.data, type_str.data);

    char buf[256];
    snprintf(buf, sizeof(buf), "struct %s", mangled.data);
    replacement = parse_template(ctx->grammar, "type_specifier", buf);
    if (!replacement) {
      replacement = ncc_xform_make_token_node(NCC_TOK_IDENTIFIER, mangled.data,
                                              line, col);
    }

    ncc_free(mangled.data);
  }

  ncc_free(type_str.data);
  return replacement;
}

// ============================================================================
// Transform: _some/_none/_is_some/_is_none/_unwrap on postfix_expression
// ============================================================================

// Extract callee name from a postfix_expression that looks like a call.
// Returns the name if it's one of our keywords, else nullptr.
static const char *get_option_callee(ncc_parse_tree_t *node, size_t nc) {
  if (nc < 3) {
    return nullptr;
  }

  ncc_parse_tree_t *callee = ncc_tree_child(node, 0);
  if (!callee) {
    return nullptr;
  }

  const char *name = nullptr;
  if (ncc_tree_is_leaf(callee)) {
    name = ncc_xform_leaf_text(callee);
  } else {
    size_t cnc = ncc_tree_num_children(callee);
    if (cnc == 1) {
      ncc_parse_tree_t *inner = ncc_tree_child(callee, 0);
      if (inner && ncc_tree_is_leaf(inner)) {
        name = ncc_xform_leaf_text(inner);
      }
    }
  }

  if (!name) {
    return nullptr;
  }

  if (strcmp(name, "_some") == 0 || strcmp(name, "_none") == 0 ||
      strcmp(name, "_is_some") == 0 || strcmp(name, "_is_none") == 0 ||
      strcmp(name, "_unwrap") == 0) {
    return name;
  }

  return nullptr;
}

// Collect arguments from an argument_expression_list.
static ncc_parse_tree_t **option_collect_args(ncc_parse_tree_t *arglist,
                                              int *nargs) {
  *nargs = 0;
  if (!arglist) {
    return nullptr;
  }

  // Count by walking the left-recursive structure.
  int count = 0;
  ncc_parse_tree_t *cur = arglist;
  while (cur && !ncc_tree_is_leaf(cur)) {
    count++;
    ncc_parse_tree_t *sub =
        ncc_xform_find_child_nt(cur, "argument_expression_list");
    if (!sub) {
      break;
    }
    cur = sub;
  }

  ncc_parse_tree_t **args = ncc_alloc_array(ncc_parse_tree_t *, (size_t)count);
  int idx = count - 1;
  cur = arglist;
  while (cur && !ncc_tree_is_leaf(cur) && idx >= 0) {
    size_t nc = ncc_tree_num_children(cur);
    for (size_t i = nc; i > 0; i--) {
      ncc_parse_tree_t *c = ncc_tree_child(cur, i - 1);
      if (c && !ncc_tree_is_leaf(c) &&
          !ncc_xform_nt_name_is(c, "argument_expression_list")) {
        args[idx--] = c;
        break;
      }
    }
    ncc_parse_tree_t *sub =
        ncc_xform_find_child_nt(cur, "argument_expression_list");
    if (!sub) {
      break;
    }
    cur = sub;
  }

  *nargs = count;
  return args;
}

// Resolve whether an expression refers to a pointer-type option.
static bool resolve_is_pointer(ncc_xform_ctx_t *ctx,
                               ncc_parse_tree_t *expr_node,
                               bool *out_is_pointer) {
  ncc_string_t expr_text = ncc_xform_node_to_text(expr_node);
  if (!expr_text.data) {
    return false;
  }

  // Trim whitespace.
  char *p = expr_text.data;
  while (*p == ' ' || *p == '\t')
    p++;
  size_t len = strlen(p);
  while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
    len--;
  char name[256];
  if (len >= sizeof(name))
    len = sizeof(name) - 1;
  memcpy(name, p, len);
  name[len] = '\0';
  ncc_free(expr_text.data);

  ncc_dict_t *meta = get_option_meta(ctx);
  bool found;
  void *val = ncc_dict_get(meta, name, &found);
  if (found) {
    *out_is_pointer = (val != nullptr);
    return true;
  }

  // Fallback: compile-time check using _Generic.
  ncc_xform_data_t *xdata = ncc_xform_get_data(ctx);
  const char *compiler = xdata ? xdata->compiler : nullptr;
  if (!compiler) {
    compiler = "cc";
  }

  char *decls = collect_file_scope_declarations(ctx, expr_node);

  char src[4096];
  snprintf(src, sizeof(src),
           "#include <stdio.h>\n"
           "%s\n"
           "int main(void) {\n"
           "    printf(\"%%d\\n\", _Generic(&(typeof((%s))){0},\n"
           "        typeof(*(%s)) **: 1,\n"
           "        default: 0));\n"
           "}\n",
           decls ? decls : "", name, name);

  ncc_free(decls);

  char *err = nullptr;
  char *output = compile_and_run(compiler, src, &err);
  ncc_free(err);

  if (output) {
    *out_is_pointer = (atoi(output) != 0);
    ncc_free(output);
    return true;
  }

  return false;
}

static ncc_parse_tree_t *xform_option_ops(ncc_xform_ctx_t *ctx,
                                          ncc_parse_tree_t *node) {
  size_t nc = ncc_tree_num_children(node);

  const char *callee = get_option_callee(node, nc);
  if (!callee) {
    return nullptr;
  }

  uint32_t line, col;
  ncc_xform_first_leaf_pos(node, &line, &col);

  // ========================================================================
  // _some(T, val) — grammar: <typeid_atom> <typeid_continuation>? ","
  // <assignment_expression>
  // ========================================================================
  if (strcmp(callee, "_some") == 0) {
    ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
    ncc_parse_tree_t *cont =
        ncc_xform_find_child_nt(node, "typeid_continuation");
    ncc_parse_tree_t *val_expr =
        ncc_xform_find_child_nt(node, "assignment_expression");

    if (!atom || !val_expr) {
      fprintf(stderr,
              "ncc: error: _some requires (type, value) "
              "(line %u, col %u)\n",
              line, col);
      exit(1);
    }

    ncc_string_t type_text = ncc_xform_extract_type_string(ctx, atom, cont);
    ncc_string_t val_text = ncc_xform_node_to_text(val_expr);
    bool is_ptr = type_string_is_pointer(type_text.data);

    char src[2048];
    if (is_ptr) {
      snprintf(src, sizeof(src), "(%s)", val_text.data);
    } else {
      char *opt_str = option_type_string(type_text.data);
      ncc_string_t mangled = ncc_type_mangle(opt_str);
      ncc_free(opt_str);
      emit_struct_decl(ctx, mangled.data, type_text.data);

      snprintf(src, sizeof(src), "((struct %s){.has_value = 1, .value = (%s)})",
               mangled.data, val_text.data);
      ncc_free(mangled.data);
    }

    ncc_free(type_text.data);
    ncc_free(val_text.data);

    return parse_template(ctx->grammar, "primary_expression", src);
  }

  // ========================================================================
  // _none(T) — grammar: <typeid_atom> <typeid_continuation>?
  // ========================================================================
  if (strcmp(callee, "_none") == 0) {
    ncc_parse_tree_t *atom = ncc_xform_find_child_nt(node, "typeid_atom");
    ncc_parse_tree_t *cont =
        ncc_xform_find_child_nt(node, "typeid_continuation");

    if (!atom) {
      fprintf(stderr,
              "ncc: error: _none requires 1 argument (type) "
              "(line %u, col %u)\n",
              line, col);
      exit(1);
    }

    ncc_string_t type_text = ncc_xform_extract_type_string(ctx, atom, cont);
    bool is_ptr = type_string_is_pointer(type_text.data);

    char src[2048];
    if (is_ptr) {
      snprintf(src, sizeof(src), "((%s)nullptr)", type_text.data);
    } else {
      char *opt_str = option_type_string(type_text.data);
      ncc_string_t mangled = ncc_type_mangle(opt_str);
      ncc_free(opt_str);
      emit_struct_decl(ctx, mangled.data, type_text.data);

      snprintf(src, sizeof(src), "((struct %s){.has_value = 0, .value = {}})",
               mangled.data);
      ncc_free(mangled.data);
    }

    ncc_free(type_text.data);

    return parse_template(ctx->grammar, "primary_expression", src);
  }

  // For _is_some/_is_none/_unwrap, use argument_expression_list as before.
  ncc_parse_tree_t *arglist =
      ncc_xform_find_child_nt(node, "argument_expression_list");
  int nargs = 0;
  ncc_parse_tree_t **args = option_collect_args(arglist, &nargs);

  // ========================================================================
  // _is_some(x) / _is_none(x)
  // ========================================================================
  if (strcmp(callee, "_is_some") == 0 || strcmp(callee, "_is_none") == 0) {
    bool is_some = (strcmp(callee, "_is_some") == 0);

    if (nargs != 1) {
      fprintf(stderr,
              "ncc: error: %s requires 1 argument "
              "(line %u, col %u)\n",
              callee, line, col);
      exit(1);
    }

    ncc_string_t expr_text = ncc_xform_node_to_text(args[0]);
    bool is_ptr = false;

    if (!resolve_is_pointer(ctx, args[0], &is_ptr)) {
      fprintf(stderr,
              "ncc: error: cannot determine type of '%s' for %s "
              "(line %u, col %u)\n",
              expr_text.data, callee, line, col);
      exit(1);
    }

    char src[2048];
    if (is_ptr) {
      snprintf(src, sizeof(src), "((%s) %s nullptr)", expr_text.data,
               is_some ? "!=" : "==");
    } else {
      snprintf(src, sizeof(src), "(%s(%s).has_value)", is_some ? "" : "!",
               expr_text.data);
    }

    ncc_free(expr_text.data);
    ncc_free(args);

    return parse_template(ctx->grammar, "primary_expression", src);
  }

  // ========================================================================
  // _unwrap(x)
  // ========================================================================
  if (strcmp(callee, "_unwrap") == 0) {
    if (nargs != 1) {
      fprintf(stderr,
              "ncc: error: _unwrap requires 1 argument "
              "(line %u, col %u)\n",
              line, col);
      exit(1);
    }

    ncc_string_t expr_text = ncc_xform_node_to_text(args[0]);
    bool is_ptr = false;

    if (!resolve_is_pointer(ctx, args[0], &is_ptr)) {
      fprintf(stderr,
              "ncc: error: cannot determine type of '%s' for _unwrap "
              "(line %u, col %u)\n",
              expr_text.data, line, col);
      exit(1);
    }

    int id = ctx->unique_id++;
    char var_name[64];
    snprintf(var_name, sizeof(var_name), "_ncc_opt_%d", id);

    char src[4096];
    if (is_ptr) {
      snprintf(src, sizeof(src),
               "({ __auto_type %s = (%s);"
               " if (%s == nullptr) { __builtin_trap(); }"
               " %s; })",
               var_name, expr_text.data, var_name, var_name);
    } else {
      snprintf(src, sizeof(src),
               "({ __auto_type %s = (%s);"
               " if (!%s.has_value) { __builtin_trap(); }"
               " %s.value; })",
               var_name, expr_text.data, var_name, var_name);
    }

    ncc_free(expr_text.data);
    ncc_free(args);

    return parse_template(ctx->grammar, "primary_expression", src);
  }

  ncc_free(args);
  return nullptr;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_option_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "synthetic_identifier", xform_option_type,
                     "option_type");
  ncc_xform_register(reg, "postfix_expression", xform_option_ops, "option_ops");
}
