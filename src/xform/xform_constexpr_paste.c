// xform_constexpr_paste.c — Transform: constexpr_paste(prefix, suffix).
//
// Concatenates two compile-time values into a synthetic identifier.
//
//   constexpr_paste("item_", 3)         → item_3
//   constexpr_paste(field_, typeid(int)) → field_<hash>
//   constexpr_paste("ncc_", "list")    → ncc_list
//
// Registered as pre-order on "synthetic_identifier" and "postfix_expression".

#include "lib/alloc.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

// Check if a string is a valid C identifier fragment (alnum + underscore).
static bool is_ident_fragment(const char *s) {
  if (!s || !*s) {
    return false;
  }
  for (const char *p = s; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '_') {
      return false;
    }
  }
  return true;
}

// Emit a subtree to text, trim whitespace, strip surrounding quotes.
static char *pprint_and_clean(ncc_grammar_t *g, ncc_parse_tree_t *node) {
  ncc_string_t raw = pprint_subtree(g, node);
  if (!raw.data) {
    return nullptr;
  }

  // Trim leading whitespace.
  char *start = raw.data;
  while (*start == ' ' || *start == '\t' || *start == '\n') {
    start++;
  }

  // Trim trailing whitespace.
  char *end = start + strlen(start);
  while (end > start &&
         (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
    end--;
  }

  size_t len = (size_t)(end - start);
  char *result = ncc_alloc_size(1, len + 1);
  memcpy(result, start, len);
  result[len] = '\0';
  ncc_free(raw.data);

  // Strip surrounding quotes if present.
  size_t rlen = strlen(result);
  if (rlen >= 2 && result[0] == '"' && result[rlen - 1] == '"') {
    memmove(result, result + 1, rlen - 2);
    result[rlen - 2] = '\0';
  }

  return result;
}

// Try to evaluate an expression as an integer.
// Fast path: strtoll. Fallback: compile_and_run.
static bool try_eval_integer(const char *compiler, const char *expr,
                             long long *out) {
  char *endptr;
  long long value;

  errno = 0;
  value = strtoll(expr, &endptr, 0);

  // Skip trailing whitespace.
  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n') {
    endptr++;
  }

  if (errno == 0 && *endptr == '\0' && endptr != expr) {
    *out = value;
    return true;
  }

  if (!compiler) {
    return false;
  }

  // Fallback: compile and run.
  char *source = nullptr;
  size_t source_size;
  FILE *sf = open_memstream(&source, &source_size);
  fprintf(sf, "#include <stdio.h>\n");
  fprintf(sf, "#include <stddef.h>\n");
  fprintf(sf, "#include <stdint.h>\n");
  fprintf(sf, "int main(void) {\n");
  fprintf(sf, "    printf(\"%%lld\", (long long)(%s));\n", expr);
  fprintf(sf, "    return 0;\n");
  fprintf(sf, "}\n");
  fclose(sf);

  char *err_msg = nullptr;
  char *result = compile_and_run(compiler, source, &err_msg);
  ncc_free(source);
  ncc_free(err_msg);

  if (!result) {
    return false;
  }

  errno = 0;
  value = strtoll(result, &endptr, 10);

  // Skip trailing whitespace.
  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n') {
    endptr++;
  }

  bool ok = (errno == 0 && *endptr == '\0' && endptr != result);
  if (ok) {
    *out = value;
  }
  ncc_free(result);
  return ok;
}

// get_first_leaf_text — uses shared helper.
static const char *get_first_leaf_text(ncc_parse_tree_t *node) {
  return ncc_xform_get_first_leaf_text(node);
}

// ============================================================================
// Main transform
//
// Handles two tree shapes:
//
// Shape 1 — synthetic_identifier (grammar picks constexpr_paste rule):
//   synthetic_identifier
//     ├── "constexpr_paste"  (keyword leaf)
//     ├── "("
//     ├── argument_expression_list
//     └── ")"
//
// Shape 2 — postfix_expression CALL (grammar picks identifier + call):
//   postfix_expression
//     ├── postfix_expression → ... → "constexpr_paste"
//     ├── "("
//     ├── argument_expression_list
//     └── ")"
// ============================================================================

static ncc_parse_tree_t *xform_constexpr_paste(ncc_xform_ctx_t *ctx,
                                               ncc_parse_tree_t *node) {
  size_t nc = ncc_tree_num_children(node);
  if (nc < 3) {
    return nullptr;
  }

  // Find the callee name — either a direct leaf child[0] or buried
  // in a callee subtree.
  const char *callee = get_first_leaf_text(ncc_tree_child(node, 0));
  if (!callee || strcmp(callee, "constexpr_paste") != 0) {
    return nullptr;
  }

  // Verify "(" is child[1] (quick filter).
  ncc_parse_tree_t *child1 = ncc_tree_child(node, 1);
  if (!child1 || !ncc_xform_leaf_text_eq(child1, "(")) {
    return nullptr;
  }

  uint32_t line, col_pos;
  ncc_xform_first_leaf_pos(node, &line, &col_pos);

  ncc_xform_data_t *xdata = ncc_xform_get_data(ctx);
  const char *compiler = xdata ? xdata->compiler : nullptr;

  // Find argument_expression_list child (may be wrapped in group node).
  ncc_parse_tree_t *arglist =
      ncc_xform_find_child_nt(node, "argument_expression_list");

  if (!arglist) {
    fprintf(stderr, "ncc: %u:%u: constexpr_paste: no argument list found\n",
            line, col_pos);
    exit(1);
  }

  int nargs = 0;
  ncc_parse_tree_t **args = collect_arguments(arglist, &nargs);

  if (nargs != 2) {
    fprintf(stderr, "ncc: %u:%u: constexpr_paste expects 2 arguments, got %d\n",
            line, col_pos, nargs);
    ncc_free(args);
    exit(1);
  }

  // Process prefix.
  char *prefix = pprint_and_clean(ctx->grammar, args[0]);
  if (!prefix || !*prefix) {
    fprintf(stderr, "ncc: %u:%u: constexpr_paste: cannot emit first argument\n",
            line, col_pos);
    ncc_free(args);
    ncc_free(prefix);
    exit(1);
  }

  if (!is_ident_fragment(prefix)) {
    fprintf(stderr,
            "ncc: %u:%u: constexpr_paste: first argument '%s' is not a "
            "valid identifier fragment\n",
            line, col_pos, prefix);
    ncc_free(args);
    ncc_free(prefix);
    exit(1);
  }

  // Process suffix.
  char *suffix_text = pprint_and_clean(ctx->grammar, args[1]);
  if (!suffix_text || !*suffix_text) {
    fprintf(stderr,
            "ncc: %u:%u: constexpr_paste: cannot emit second argument\n", line,
            col_pos);
    ncc_free(args);
    ncc_free(prefix);
    ncc_free(suffix_text);
    exit(1);
  }

  // Build the concatenated identifier.
  char id_buf[512];
  int iret;

  long long int_val;
  if (try_eval_integer(compiler, suffix_text, &int_val)) {
    iret = snprintf(id_buf, sizeof(id_buf), "%s%lld", prefix, int_val);
  } else if (is_ident_fragment(suffix_text)) {
    iret = snprintf(id_buf, sizeof(id_buf), "%s%s", prefix, suffix_text);
  } else {
    fprintf(stderr,
            "ncc: %u:%u: constexpr_paste: second argument '%s' is "
            "neither an integer nor a valid identifier fragment\n",
            line, col_pos, suffix_text);
    ncc_free(args);
    ncc_free(prefix);
    ncc_free(suffix_text);
    exit(1);
  }

  ncc_free(prefix);
  ncc_free(suffix_text);
  ncc_free(args);

  if (iret < 0 || (size_t)iret >= sizeof(id_buf)) {
    fprintf(stderr, "ncc: %u:%u: constexpr_paste: identifier too long\n", line,
            col_pos);
    exit(1);
  }

  // Build replacement: a single identifier token node.
  ncc_parse_tree_t *replacement =
      ncc_xform_make_token_node(NCC_TOK_IDENTIFIER, id_buf, line, col_pos);

  if (!replacement) {
    fprintf(stderr,
            "ncc: %u:%u: constexpr_paste: failed to create token node\n", line,
            col_pos);
    exit(1);
  }

  ctx->nodes_replaced++;
  return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_constexpr_paste_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "synthetic_identifier", xform_constexpr_paste,
                     "constexpr_paste");
  ncc_xform_register(reg, "postfix_expression", xform_constexpr_paste,
                     "constexpr_paste");
}
