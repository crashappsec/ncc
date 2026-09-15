// xform_constexpr.c — Compile-time evaluation transforms.
//
// Transforms constexpr_eval, constexpr_max, constexpr_min, constexpr_strcmp,
// constexpr_strlen pseudo-functions into integer literals by compiling and
// running a helper program at compile time.
//
//   constexpr_eval(sizeof(my_struct))      → 48LL
//   constexpr_max(sizeof(A), sizeof(B))    → 128LL
//   constexpr_min(sizeof(char), sizeof(int)) → 1LL
//   constexpr_strcmp("abc", "def")          → -1LL
//   constexpr_strlen("hello")              → 5LL
//
// Registered as pre-order on "postfix_expression".

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "parse/emit.h"
#include "util/platform.h"
#include "util/sha256.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

// ============================================================================
// Helpers: parse template (wrapper around shared helper)
// ============================================================================

static ncc_parse_tree_t *parse_template(ncc_grammar_t *g, const char *nt_name,
                                        const char *src) {
  return ncc_xform_parse_source(g, nt_name, src, "xform_constexpr");
}

// ============================================================================
// compile_and_run — compile a C program, run it, return stdout
// ============================================================================

static void set_helper_launch_error(char **err_out, const char *phase,
                                    const char *detail) {
  if (!err_out) {
    return;
  }

  ncc_buffer_t *buf = ncc_buffer_empty();
  ncc_buffer_printf(buf, "helper %s launch failed", phase);
  if (detail && detail[0]) {
    ncc_buffer_puts(buf, ": ");
    ncc_buffer_puts(buf, detail);
    if (detail[strlen(detail) - 1] != '\n') {
      ncc_buffer_putc(buf, '\n');
    }
  } else {
    ncc_buffer_putc(buf, '\n');
  }
  *err_out = ncc_buffer_take(buf);
}

static void set_helper_exit_error(char **err_out, const char *phase,
                                  int exit_status, const char *stderr_data,
                                  size_t stderr_len) {
  if (!err_out) {
    return;
  }

  ncc_buffer_t *buf = ncc_buffer_empty();
  ncc_buffer_printf(buf, "helper %s failed with exit status %d",
                    phase, exit_status);
  if (stderr_data && stderr_len > 0) {
    ncc_buffer_puts(buf, ":\n");
    ncc_buffer_append(buf, stderr_data, stderr_len);
    if (stderr_data[stderr_len - 1] != '\n') {
      ncc_buffer_putc(buf, '\n');
    }
  } else {
    ncc_buffer_putc(buf, '\n');
  }
  *err_out = ncc_buffer_take(buf);
}


// ============================================================================
// Constexpr evaluation cache
// ============================================================================
//
// compile_and_run builds a self-contained program from `source` and returns its
// stdout, so the source text and the compiler that builds it determine the
// result completely: nothing about the surrounding translation unit can change
// the answer. That makes the pair a sound cache key. Headers push the same
// expressions through every TU that includes them, so one build evaluates the
// same program many times over.
//
// The key covers the source, the compiler's path/size/mtime, the fixed helper
// flags, the host, and a format version. A compiler upgrade, a flag change or a
// format change therefore misses rather than returning a stale answer. Only
// successful runs are stored; a failure re-runs so its diagnostic is produced
// fresh.
//
// NCC_CONSTEXPR_CACHE names the directory. Setting it to "0", "off" or "" turns
// both tiers off, for checking a cached answer against a recomputed one.

// The helper runs on this machine, so an entry is only valid for the same
// host shape. Kept coarse on purpose: it is part of the key, not a check.
#if defined(__APPLE__) && defined(__aarch64__)
#define NCC_CE_HOST_TAG "darwin-arm64"
#elif defined(__APPLE__)
#define NCC_CE_HOST_TAG "darwin-x86_64"
#elif defined(_WIN32)
#define NCC_CE_HOST_TAG "windows"
#elif defined(__linux__) && defined(__aarch64__)
#define NCC_CE_HOST_TAG "linux-arm64"
#elif defined(__linux__)
#define NCC_CE_HOST_TAG "linux-x86_64"
#else
#define NCC_CE_HOST_TAG "unknown-host"
#endif

#define NCC_CE_CACHE_VERSION "ncc-constexpr-2"
#define NCC_CE_HELPER_FLAGS  "-x c -std=gnu23 -w"

static ncc_dict_t ce_memo;
static bool       ce_memo_ready = false;
static int        ce_disk_state = 0;  // 0 unknown, 1 on, -1 off
static char      *ce_disk_dir   = nullptr;

static char *ce_dup(const char *s, size_t n) {
  char *d = (char *)ncc_alloc_array(char, n + 1);
  memcpy(d, s, n);
  d[n] = '\0';
  return d;
}

// The compiler often arrives as a bare name ("cc"), which cannot be stat'd.
// Resolve it against PATH so the key can carry its identity; an unresolvable
// compiler yields nullptr and disables caching rather than weakening the key.
static const char *ce_resolve_compiler(const char *compiler) {
  static const char *resolved = nullptr;
  static const char *resolved_for = nullptr;

  if (!compiler) {
    return nullptr;
  }

  if (resolved_for == compiler) {
    return resolved;
  }

  resolved_for = compiler;
  resolved     = nullptr;

  if (strchr(compiler, '/')) {
    resolved = compiler;
    return resolved;
  }

  const char *path = getenv("PATH");

  if (!path) {
    return nullptr;
  }

  size_t nlen = strlen(compiler);

  for (const char *p = path; *p;) {
    const char *sep = strchr(p, ':');
    size_t      dlen = sep ? (size_t)(sep - p) : strlen(p);

    if (dlen) {
      char *cand = (char *)ncc_alloc_array(char, dlen + nlen + 2);
      memcpy(cand, p, dlen);
      cand[dlen] = '/';
      memcpy(cand + dlen + 1, compiler, nlen + 1);

      if (access(cand, X_OK) == 0) {
        resolved = cand;
        return resolved;
      }
      ncc_free(cand);
    }

    if (!sep) {
      break;
    }
    p = sep + 1;
  }

  return nullptr;
}

// Hex of sha256(version | flags | host | compiler identity | source), or false
// when the compiler cannot be resolved and stat'd -- without its identity an
// upgrade could not invalidate the entry, so caching is refused, not risked.
static bool ce_cache_key(const char *compiler, const char *source,
                         char out[65]) {
  struct stat st;

  compiler = ce_resolve_compiler(compiler);

  if (!compiler || !source || stat(compiler, &st) != 0) {
    return false;
  }

  ncc_sha256_ctx_t ctx;
  ncc_sha256_init(&ctx);

  const char *v = NCC_CE_CACHE_VERSION "\0" NCC_CE_HELPER_FLAGS "\0";
  ncc_sha256_update(&ctx, v, sizeof(NCC_CE_CACHE_VERSION)
                                + sizeof(NCC_CE_HELPER_FLAGS));

  // The helper is compiled AND executed here, so the host decides the answer.
  const char *host = NCC_CE_HOST_TAG;
  ncc_sha256_update(&ctx, host, strlen(host) + 1);

  ncc_sha256_update(&ctx, compiler, strlen(compiler) + 1);
  uint64_t sz = (uint64_t)st.st_size;
  uint64_t mt = (uint64_t)st.st_mtime;
  ncc_sha256_update(&ctx, &sz, sizeof(sz));
  ncc_sha256_update(&ctx, &mt, sizeof(mt));
  ncc_sha256_update(&ctx, source, strlen(source) + 1);

  ncc_sha256_digest_t d;
  ncc_sha256_finalize(&ctx, d);

  for (int i = 0; i < NCC_SHA256_DIGEST_WORDS; i++) {
    snprintf(out + i * 8, 9, "%08x", d[i]);
  }
  out[64] = '\0';

  return true;
}

static bool ce_disk_enabled(void) {
  if (ce_disk_state != 0) {
    return ce_disk_state > 0;
  }

  const char *env = getenv("NCC_CONSTEXPR_CACHE");

  if (env && (!*env || !strcmp(env, "0") || !strcmp(env, "off"))) {
    ce_disk_state = -1;
    return false;
  }

  if (env) {
    ce_disk_dir = ce_dup(env, strlen(env));
  } else {
    const char *base = getenv("XDG_CACHE_HOME");
    char       *tmp  = nullptr;

    if (!base || !*base) {
      const char *home = getenv("HOME");

      if (!home || !*home) {
        ce_disk_state = -1;
        return false;
      }
      tmp  = ncc_platform_join_path(home, ".cache");
      base = tmp;
    }

    char *n = ncc_platform_join_path(base, "ncc");
    ce_disk_dir = ncc_platform_join_path(n, "constexpr");
    ncc_free(n);
    ncc_free(tmp);
  }

  ce_disk_state = ce_disk_dir ? 1 : -1;

  return ce_disk_state > 0;
}

static char *ce_entry_path(const char *key) {
  // Shard on the first two hex digits so one directory does not collect every
  // entry in the build.
  char shard[3] = {key[0], key[1], '\0'};
  char *dir     = ncc_platform_join_path(ce_disk_dir, shard);

  if (!dir) {
    return nullptr;
  }

  ncc_platform_mkdir(ce_disk_dir);
  ncc_platform_mkdir(dir);

  char *p = ncc_platform_join_path(dir, key + 2);
  ncc_free(dir);

  return p;
}

static char *ce_disk_get(const char *key) {
  if (!ce_disk_enabled()) {
    return nullptr;
  }

  char *path = ce_entry_path(key);

  if (!path) {
    return nullptr;
  }

  char *out = nullptr;
  FILE *f   = fopen(path, "rb");

  if (f) {
    if (!fseek(f, 0, SEEK_END)) {
      long n = ftell(f);

      if (n >= 0 && !fseek(f, 0, SEEK_SET)) {
        char *buf = (char *)ncc_alloc_array(char, (size_t)n + 1);

        if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
          buf[n] = '\0';
          out    = buf;
        } else {
          ncc_free(buf);
        }
      }
    }
    fclose(f);
  }

  ncc_free(path);

  return out;
}

// Written to a unique temp name and renamed, so a concurrent build never
// observes a half-written entry.
static void ce_disk_put(const char *key, const char *value) {
  if (!ce_disk_enabled()) {
    return;
  }

  char *path = ce_entry_path(key);

  if (!path) {
    return;
  }

  char tmp[64];
  snprintf(tmp, sizeof(tmp), ".tmp.%ld", (long)getpid());

  size_t n    = strlen(path) + strlen(tmp) + 1;
  char  *tpath = (char *)ncc_alloc_array(char, n);
  snprintf(tpath, n, "%s%s", path, tmp);

  char *werr = nullptr;


  if (ncc_platform_write_file(tpath, value, strlen(value), &werr)) {
    if (rename(tpath, path) != 0) {
      ncc_platform_remove_file(tpath);
    }
  } else {
    // A cache that cannot be written is not an error: the answer was already
    // computed, so drop the entry and carry on.
    ncc_free(werr);
  }

  ncc_free(tpath);
  ncc_free(path);
}

static char *ce_memo_get(const char *key) {
  if (!ce_memo_ready) {
    return nullptr;
  }

  bool  found = false;
  void *v     = ncc_dict_get(&ce_memo, (void *)key, &found);

  return found && v ? ce_dup((const char *)v, strlen((const char *)v))
                    : nullptr;
}

static void ce_memo_put(const char *key, const char *value) {
  if (!ce_memo_ready) {
    ncc_dict_init(&ce_memo, ncc_hash_cstring, ncc_dict_cstr_eq);
    ce_memo_ready = true;
  }

  ncc_dict_put(&ce_memo, ce_dup(key, strlen(key)),
               ce_dup(value, strlen(value)));
}

// `deterministic` reports whether a failure was the helper itself exiting
// nonzero (same program, same answer every time) rather than the toolchain
// failing to launch. Only the former is cacheable; a launch failure is an
// environment problem that must be retried, not remembered.
static char *compile_and_run_uncached(const char *compiler, const char *source,
                                      char **err_out, bool *deterministic);

char *compile_and_run(const char *compiler, const char *source,
                      char **err_out) {
  if (err_out) {
    *err_out = nullptr;
  }

  char key[65];
  bool keyed = ce_cache_key(compiler, source, key);



  if (keyed) {
    char *hit = ce_memo_get(key);

    if (!hit) {
      hit = ce_disk_get(key);

      if (hit) {
        ce_memo_put(key, hit);
      }
    }

    if (hit) {
      char *out = nullptr;

      if (hit[0] == 'S') {
        out = ce_dup(hit + 1, strlen(hit + 1));
      } else if (err_out) {
        *err_out = ce_dup(hit + 1, strlen(hit + 1));
      }

      ncc_free(hit);

      return out;
    }
  }

  bool  deterministic = false;
  char *result = compile_and_run_uncached(compiler, source, err_out,
                                          &deterministic);

  if (keyed && (result || deterministic)) {
    ncc_buffer_t *ent = ncc_buffer_empty();

    ncc_buffer_putc(ent, result ? 'S' : 'F');
    ncc_buffer_puts(ent, result ? result : (err_out && *err_out ? *err_out : ""));

    char *blob = ncc_buffer_take(ent);
    ce_memo_put(key, blob);
    ce_disk_put(key, blob);
    ncc_free(blob);
  }

  return result;
}

static char *compile_and_run_uncached(const char *compiler, const char *source,
                                      char **err_out, bool *deterministic) {
  if (deterministic) {
    *deterministic = false;
  }

  ncc_temp_workspace_t tmp = {0};
  char *src_path = nullptr;
  char *bin_path = nullptr;
  char *result = nullptr;
  ncc_process_result_t compile_proc = {0};
  ncc_process_result_t run_proc = {0};

  if (!ncc_temp_workspace_create(&tmp, "ncc_ce_", err_out)) {
    return nullptr;
  }

  src_path = ncc_temp_workspace_join(&tmp, "src.c");
#ifdef _WIN32
  bin_path = ncc_temp_workspace_join(&tmp, "bin.exe");
#else
  bin_path = ncc_temp_workspace_join(&tmp, "bin");
#endif

  if (!src_path || !bin_path) {
    goto cleanup;
  }

  char *write_err = nullptr;
  if (!ncc_platform_write_file(src_path, source, strlen(source), &write_err)) {
    if (err_out) {
      *err_out = write_err;
    } else {
      ncc_free(write_err);
    }
    goto cleanup;
  }

  const char *compile_argv[] = {
      compiler, "-x", "c", "-std=gnu23", "-w", "-o", bin_path, src_path,
      nullptr,
  };
  ncc_process_spec_t compile_spec = {
      .program = compiler,
      .argv = compile_argv,
      .capture_stderr = true,
  };

  if (!ncc_process_run(&compile_spec, &compile_proc)) {
    set_helper_launch_error(err_out, "compile", compile_proc.stderr_data);
    goto cleanup;
  }

  if (compile_proc.exit_code != 0) {
    if (deterministic) {
      *deterministic = true;
    }
    set_helper_exit_error(err_out, "compile", compile_proc.exit_code,
                          compile_proc.stderr_data,
                          compile_proc.stderr_len);
    goto cleanup;
  }

  ncc_process_result_free(&compile_proc);

  const char *run_argv[] = {bin_path, nullptr};
  ncc_process_spec_t run_spec = {
      .program = bin_path,
      .argv = run_argv,
      .capture_stdout = true,
      .capture_stderr = true,
  };

  if (!ncc_process_run(&run_spec, &run_proc)) {
    set_helper_launch_error(err_out, "execution", run_proc.stderr_data);
    goto cleanup;
  }

  if (run_proc.exit_code != 0) {
    if (deterministic) {
      *deterministic = true;
    }
    set_helper_exit_error(err_out, "execution", run_proc.exit_code,
                          run_proc.stderr_data, run_proc.stderr_len);
    goto cleanup;
  }

  result = run_proc.stdout_data;
  run_proc.stdout_data = nullptr;
  run_proc.stdout_len = 0;

cleanup:
  ncc_process_result_free(&compile_proc);
  ncc_process_result_free(&run_proc);
  ncc_free(src_path);
  ncc_free(bin_path);
  ncc_temp_workspace_cleanup(&tmp);
  return result;
}

// ============================================================================
// pprint_subtree — emit a parse subtree as C source via ncc_pprint
// ============================================================================

ncc_string_t pprint_subtree(ncc_grammar_t *g, ncc_parse_tree_t *node) {
  ncc_pprint_opts_t opts = {
      .line_width = 200,
      .indent_size = 4,
      .indent_style = NCC_PPRINT_SPACES,
      .use_unicode_width = false,
      .out = nullptr,
      .newline = "\n",
      .style = nullptr,
  };

  return ncc_pprint(g, node, opts);
}

// ============================================================================
// collect_arguments — extract args from argument_expression_list
// ============================================================================

ncc_parse_tree_t **collect_arguments(ncc_parse_tree_t *arglist, int *nargs) {
  *nargs = 0;

  if (!arglist) {
    return nullptr;
  }

  // Walk the left-recursive argument_expression_list.
  // Shape: arg_list -> arg_list "," assignment_expression
  //    or: arg_list -> assignment_expression
  //    or: arg_list -> keyword_argument (treated like assignment_expression)
  int cap = 8;
  ncc_parse_tree_t **stack = ncc_alloc_array(ncc_parse_tree_t *, (size_t)cap);
  int top = 0;

  ncc_parse_tree_t *cur = arglist;

  for (;;) {
    size_t nc = ncc_tree_num_children(cur);

    if (nc >= 3) {
      ncc_parse_tree_t *child0 = ncc_tree_child(cur, 0);

      if (ncc_xform_nt_name_is(child0, "argument_expression_list")) {
        // Left-recursive: child[0] = nested arglist, child[1] = ",",
        //                 child[2] = assignment_expression
        ncc_parse_tree_t *right_arg = ncc_tree_child(cur, 2);
        if (top >= cap) {
          cap *= 2;
          stack = ncc_realloc(stack, sizeof(ncc_parse_tree_t *) * (size_t)cap);
        }
        stack[top++] = right_arg;
        cur = child0;
        continue;
      }
    }

    // Base case: single argument (assignment_expression or keyword_argument).
    if (nc >= 1) {
      ncc_parse_tree_t *arg = ncc_tree_child(cur, 0);
      if (top >= cap) {
        cap *= 2;
        stack = ncc_realloc(stack, sizeof(ncc_parse_tree_t *) * (size_t)cap);
      }
      stack[top++] = arg;
    }
    break;
  }

  // Reverse into result array (stack is in right-to-left order).
  ncc_parse_tree_t **args = ncc_alloc_array(ncc_parse_tree_t *, (size_t)top);
  for (int i = 0; i < top; i++) {
    args[i] = stack[top - 1 - i];
  }
  *nargs = top;
  ncc_free(stack);
  return args;
}

// ============================================================================
// collect_file_scope_declarations — walk parse tree for user type definitions
// ============================================================================

// Check whether a subtree contains a specific node (pointer comparison).
static bool contains_node(ncc_parse_tree_t *root, ncc_parse_tree_t *target) {
  if (!root || !target) {
    return false;
  }
  if (root == target) {
    return true;
  }
  if (ncc_tree_is_leaf(root)) {
    return false;
  }

  size_t nc = ncc_tree_num_children(root);
  for (size_t i = 0; i < nc; i++) {
    if (contains_node(ncc_tree_child(root, i), target)) {
      return true;
    }
  }
  return false;
}

// Check if the first leaf token of a node has the system_header flag.
static bool node_in_system_header(ncc_parse_tree_t *node) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    ncc_token_info_t *tok = ncc_tree_leaf_value(node);
    return tok && tok->system_header;
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(node, i);
    if (ncc_tree_is_leaf(c)) {
      ncc_token_info_t *tok = ncc_tree_leaf_value(c);
      if (tok) {
        return tok->system_header;
      }
    } else {
      return node_in_system_header(c);
    }
  }
  return false;
}

// DFS search for a leaf with text matching `keyword` under node.
static bool has_keyword_leaf(ncc_parse_tree_t *node, const char *keyword) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    return ncc_xform_leaf_text_eq(node, keyword);
  }
  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (has_keyword_leaf(ncc_tree_child(node, i), keyword)) {
      return true;
    }
  }
  return false;
}

// Check if a declaration has a typedef keyword in its declaration_specifiers.
static bool has_typedef_keyword(ncc_parse_tree_t *decl) {
  size_t nc = ncc_tree_num_children(decl);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *c = ncc_tree_child(decl, i);
    if (ncc_xform_nt_name_is(c, "declaration_specifiers")) {
      return has_keyword_leaf(c, "typedef");
    }
  }
  return false;
}

// Check if a declaration contains a struct/union/enum definition (with body).
static bool has_struct_body(ncc_parse_tree_t *node) {
  if (!node) {
    return false;
  }
  if (ncc_tree_is_leaf(node)) {
    return false;
  }

  if (ncc_xform_nt_name_is(node, "struct_or_union_specifier") ||
      ncc_xform_nt_name_is(node, "enum_specifier")) {
    // Check if it has a "{" child — meaning it's a definition, not just
    // a forward reference.
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
      ncc_parse_tree_t *c = ncc_tree_child(node, i);
      if (ncc_tree_is_leaf(c) && ncc_xform_leaf_text_eq(c, "{")) {
        return true;
      }
    }
    return false;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    if (has_struct_body(ncc_tree_child(node, i))) {
      return true;
    }
  }
  return false;
}

// Is this an interesting declaration for constexpr? (typedef or struct def)
static bool is_type_declaration(ncc_parse_tree_t *decl) {
  return has_typedef_keyword(decl) || has_struct_body(decl);
}

// Check if a node is a group wrapper ($$group_*).
static bool is_group_wrapper(ncc_parse_tree_t *node) {
  if (!node || ncc_tree_is_leaf(node)) {
    return false;
  }
  ncc_nt_node_t pn = ncc_tree_node_value(node);
  return pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$';
}

// Recursively walk translation_unit children, flattening group wrappers,
// and collect type declarations (typedefs, struct/union/enum defs).
static void collect_decls_recursive(ncc_parse_tree_t *node,
                                    ncc_parse_tree_t *call_node,
                                    ncc_grammar_t *grammar, ncc_buffer_t *buf) {
  if (!node || ncc_tree_is_leaf(node)) {
    return;
  }

  // If this is a group wrapper, recurse into its children.
  if (is_group_wrapper(node)) {
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
      collect_decls_recursive(ncc_tree_child(node, i), call_node, grammar, buf);
    }
    return;
  }

  // This should be an external_declaration. Find its declaration child.
  ncc_parse_tree_t *decl = ncc_xform_find_child_nt(node, "declaration");
  if (!decl) {
    return;
  }

  // Skip system header declarations.
  if (node_in_system_header(decl)) {
    return;
  }

  // Skip the declaration that contains the constexpr call.
  if (call_node && contains_node(node, call_node)) {
    return;
  }

  // Only emit type declarations (typedef, struct/union/enum defs).
  if (!is_type_declaration(decl)) {
    return;
  }

  ncc_string_t text = pprint_subtree(grammar, node);
  if (text.data) {
    ncc_buffer_puts(buf, text.data);
    ncc_buffer_putc(buf, '\n');
    ncc_free(text.data);
  }
}

char *collect_file_scope_declarations(ncc_xform_ctx_t *ctx,
                                      ncc_parse_tree_t *call_node) {
  ncc_parse_tree_t *root = ctx->root;
  if (!root) {
    return strdup("");
  }

  ncc_buffer_t *buf = ncc_buffer_empty();

  size_t nc = ncc_tree_num_children(root);
  for (size_t i = 0; i < nc; i++) {
    collect_decls_recursive(ncc_tree_child(root, i), call_node, ctx->grammar,
                            buf);
  }

  return ncc_buffer_take(buf);
}

// ============================================================================
// strip_line_directives — safety filter for # NNN lines
// ============================================================================

static char *strip_line_directives(const char *src) {
  if (!src) {
    return strdup("");
  }

  ncc_buffer_t *buf = ncc_buffer_empty();

  const char *p = src;
  while (*p) {
    if (*p == '#' && (p == src || p[-1] == '\n')) {
      const char *s = p + 1;
      while (*s == ' ' || *s == '\t') {
        s++;
      }
      if (isdigit((unsigned char)*s)) {
        // Skip the entire line.
        while (*p && *p != '\n') {
          p++;
        }
        if (*p == '\n') {
          p++;
        }
        continue;
      }
    }
    ncc_buffer_putc(buf, *p);
    p++;
  }

  return ncc_buffer_take(buf);
}

// ============================================================================
// get_callee_name — extract the function name from a postfix_expression CALL
// ============================================================================

// Walk to the leftmost leaf of a subtree to find the callee token.
static const char *get_first_leaf_text(ncc_parse_tree_t *node) {
  return ncc_xform_get_first_leaf_text(node);
}

// ============================================================================
// Main transform: constexpr_eval, constexpr_max, constexpr_min,
//                 constexpr_strcmp, constexpr_strlen
//
// Registered as pre-order on "postfix_expression".
// Matches the function call tree shape:
//   postfix_expression
//     ├── postfix_expression → ... → "constexpr_eval"
//     ├── "("
//     ├── argument_expression_list
//     └── ")"
// ============================================================================

static ncc_parse_tree_t *xform_constexpr(ncc_xform_ctx_t *ctx,
                                         ncc_parse_tree_t *node) {

  // A function call has 4 children: callee, "(", arglist, ")".
  size_t nc = ncc_tree_num_children(node);
  if (nc < 3) {
    return nullptr;
  }

  // Check child[1] is "(" — quick filter for CALL shape.
  ncc_parse_tree_t *child1 = ncc_tree_child(node, 1);
  if (!child1 || !ncc_xform_leaf_text_eq(child1, "(")) {
    return nullptr;
  }

  // Extract callee name from child[0] (dig to first leaf).
  ncc_parse_tree_t *callee_node = ncc_tree_child(node, 0);
  const char *callee = get_first_leaf_text(callee_node);
  if (!callee) {
    return nullptr;
  }

  enum {
    CE_NONE,
    CE_EVAL,
    CE_MAX,
    CE_MIN,
    CE_STRCMP,
    CE_STRLEN,
  } mode = CE_NONE;

  if (strcmp(callee, "constexpr_eval") == 0) {
    mode = CE_EVAL;
  } else if (strcmp(callee, "constexpr_max") == 0) {
    mode = CE_MAX;
  } else if (strcmp(callee, "constexpr_min") == 0) {
    mode = CE_MIN;
  } else if (strcmp(callee, "constexpr_strcmp") == 0) {
    mode = CE_STRCMP;
  } else if (strcmp(callee, "constexpr_strlen") == 0) {
    mode = CE_STRLEN;
  }

  if (mode == CE_NONE) {
    return nullptr;
  }

  uint32_t line, col;
  ncc_xform_first_leaf_pos(node, &line, &col);

  // Extract compiler path and constexpr headers from user_data.
  ncc_xform_data_t *xdata = ncc_xform_get_data(ctx);
  const char *compiler = xdata ? xdata->compiler : nullptr;
  if (!compiler) {
    fprintf(stderr, "ncc: %u:%u: constexpr: no compiler available\n", line,
            col);
    exit(1);
  }

  // Find the argument_expression_list (may be wrapped in a group node).
  ncc_parse_tree_t *arglist =
      ncc_xform_find_child_nt(node, "argument_expression_list");

  if (!arglist) {
    fprintf(stderr, "ncc: %u:%u: constexpr: no argument list found\n", line,
            col);
    exit(1);
  }

  // Collect arguments.
  int nargs = 0;
  ncc_parse_tree_t **args = collect_arguments(arglist, &nargs);

  // Validate argument counts.
  if (mode == CE_EVAL || mode == CE_STRLEN) {
    if (nargs != 1) {
      fprintf(stderr, "ncc: %u:%u: constexpr_%s expects 1 argument, got %d\n",
              line, col, mode == CE_EVAL ? "eval" : "strlen", nargs);
      ncc_free(args);
      exit(1);
    }
  } else if (mode == CE_STRCMP) {
    if (nargs != 2) {
      fprintf(stderr,
              "ncc: %u:%u: constexpr_strcmp expects 2 arguments, got %d\n",
              line, col, nargs);
      ncc_free(args);
      exit(1);
    }
  } else {
    // max/min need >= 2.
    if (nargs < 2) {
      fprintf(stderr,
              "ncc: %u:%u: constexpr_%s expects >= 2 arguments, got %d\n", line,
              col, mode == CE_MAX ? "max" : "min", nargs);
      ncc_free(args);
      exit(1);
    }
  }

  // Build the program body.
  ncc_buffer_t *body_buf = ncc_buffer_empty();
  ncc_buffer_puts(body_buf, "int main(void) {\n");

  if (mode == CE_EVAL) {
    ncc_string_t expr_str = pprint_subtree(ctx->grammar, args[0]);
    char *clean = strip_line_directives(expr_str.data);
    ncc_buffer_printf(body_buf,
                      "    printf(\"%%lld\\n\", (long long)(%s));\n", clean);
    ncc_free(expr_str.data);
    ncc_free(clean);
  } else if (mode == CE_STRLEN) {
    ncc_string_t expr_str = pprint_subtree(ctx->grammar, args[0]);
    char *clean = strip_line_directives(expr_str.data);
    ncc_buffer_printf(body_buf,
                      "    printf(\"%%lld\\n\", (long long)strlen(%s));\n",
                      clean);
    ncc_free(expr_str.data);
    ncc_free(clean);
  } else if (mode == CE_STRCMP) {
    ncc_string_t e0 = pprint_subtree(ctx->grammar, args[0]);
    char *c0 = strip_line_directives(e0.data);
    ncc_string_t e1 = pprint_subtree(ctx->grammar, args[1]);
    char *c1 = strip_line_directives(e1.data);
    ncc_buffer_printf(body_buf,
                      "    printf(\"%%lld\\n\", (long long)strcmp(%s, %s));\n",
                      c0, c1);
    ncc_free(e0.data);
    ncc_free(c0);
    ncc_free(e1.data);
    ncc_free(c1);
  } else {
    // max or min.
    for (int i = 0; i < nargs; i++) {
      ncc_string_t expr_str = pprint_subtree(ctx->grammar, args[i]);
      char *clean = strip_line_directives(expr_str.data);
      ncc_buffer_printf(body_buf, "    long long _v%d = (long long)(%s);\n", i,
                        clean);
      ncc_free(expr_str.data);
      ncc_free(clean);
    }
    ncc_buffer_puts(body_buf, "    long long _result = _v0;\n");
    const char *cmp = (mode == CE_MAX) ? ">" : "<";
    for (int i = 1; i < nargs; i++) {
      ncc_buffer_printf(body_buf,
                        "    if (_v%d %s _result) _result = _v%d;\n", i, cmp,
                        i);
    }
    ncc_buffer_puts(body_buf, "    printf(\"%lld\\n\", _result);\n");
  }

  ncc_buffer_puts(body_buf, "    return 0;\n}\n");
  char *body = ncc_buffer_take(body_buf);

  // Build headers preamble.
  ncc_buffer_t *hdr_buf = ncc_buffer_empty();

  ncc_buffer_puts(hdr_buf, "#include <stdio.h>\n");
  ncc_buffer_puts(hdr_buf, "#include <stdint.h>\n");
  ncc_buffer_puts(hdr_buf, "#include <stddef.h>\n");
  ncc_buffer_puts(hdr_buf, "#include <limits.h>\n");
  ncc_buffer_puts(hdr_buf, "#include <stdalign.h>\n");
  ncc_buffer_puts(hdr_buf, "#include <string.h>\n");
  ncc_buffer_puts(hdr_buf, "#ifndef _WIN32\n");
  ncc_buffer_puts(hdr_buf, "#include <pthread.h>\n");
  ncc_buffer_puts(hdr_buf, "#endif\n");

  // --ncc-constexpr-include flag takes precedence over env var.
  const char *extra = (xdata && xdata->constexpr_headers)
                          ? xdata->constexpr_headers
                          : getenv("NCC_CONSTEXPR_HEADERS");
  if (extra) {
    char *copy = strdup(extra);
    char *tok = strtok(copy, ",");
    while (tok) {
      while (*tok == ' ') {
        tok++;
      }
      if (*tok) {
        size_t tlen = strlen(tok);
        while (tlen > 0 && tok[tlen - 1] == ' ') {
          tok[--tlen] = '\0';
        }
        bool valid = false;
        if (tlen >= 3) {
          char first = tok[0];
          char last = tok[tlen - 1];
          if ((first == '<' && last == '>') || (first == '"' && last == '"')) {
            valid = true;
            for (size_t i = 1; i < tlen - 1; i++) {
              if (tok[i] == '\n' || tok[i] == '\r') {
                valid = false;
                break;
              }
            }
          }
        }
        if (valid) {
          ncc_buffer_printf(hdr_buf, "#include %s\n", tok);
        } else {
          fprintf(stderr,
                  "ncc: constexpr: invalid NCC_CONSTEXPR_HEADERS "
                  "token: '%s' (must be <...> or \"...\")\n",
                  tok);
          exit(1);
        }
      }
      tok = strtok(nullptr, ",");
    }
    ncc_free(copy);
  }

  char *headers = ncc_buffer_take(hdr_buf);

  // Two-try strategy: first without declarations, then with.
  ncc_buffer_t *prog_buf = ncc_buffer_empty();
  ncc_buffer_puts(prog_buf, headers);
  ncc_buffer_puts(prog_buf, body);
  char *program = ncc_buffer_take(prog_buf);

  char *compile_err = nullptr;
  char *output = compile_and_run(compiler, program, &compile_err);

  if (!output && (!compile_err ||
                  strstr(compile_err, "helper execution") == nullptr)) {
    // Retry with file-scope declarations.
    ncc_free(program);
    ncc_free(compile_err);
    compile_err = nullptr;

    char *raw_decls = collect_file_scope_declarations(ctx, node);
    char *decls = strip_line_directives(raw_decls);
    ncc_free(raw_decls);

    if (decls && *decls) {
      prog_buf = ncc_buffer_empty();
      ncc_buffer_puts(prog_buf, headers);
      ncc_buffer_puts(prog_buf, decls);
      ncc_buffer_putc(prog_buf, '\n');
      ncc_buffer_puts(prog_buf, body);
      program = ncc_buffer_take(prog_buf);

      output = compile_and_run(compiler, program, &compile_err);
      ncc_free(program);
    } else {
      program = nullptr;
    }

    ncc_free(decls);
  } else {
    ncc_free(program);
  }

  if (!output) {
    if (compile_err) {
      fprintf(stderr, "ncc: %u:%u: constexpr: %s", line, col, compile_err);
      ncc_free(compile_err);
    } else {
      fprintf(stderr, "ncc: %u:%u: constexpr: compilation/execution failed\n",
              line, col);
    }
    ncc_free(headers);
    ncc_free(body);
    ncc_free(args);
    exit(1);
  }

  ncc_free(headers);
  ncc_free(body);
  ncc_free(compile_err);

  // Parse the integer result.
  char *endptr;
  long long value = strtoll(output, &endptr, 10);
  if (endptr == output) {
    fprintf(stderr, "ncc: %u:%u: constexpr: failed to parse result '%s'\n",
            line, col, output);
    ncc_free(output);
    ncc_free(args);
    exit(1);
  }
  ncc_free(output);
  ncc_free(args);

  // Format as literal with LL suffix. Negative values need parentheses
  // because `-1LL` is a unary expression, not a primary expression.
  char result_str[64];
  if (value < 0) {
    snprintf(result_str, sizeof(result_str), "(%lldLL)", value);
  } else {
    snprintf(result_str, sizeof(result_str), "%lldLL", value);
  }

  // Parse as primary_expression.
  ncc_parse_tree_t *replacement =
      parse_template(ctx->grammar, "primary_expression", result_str);

  if (!replacement) {
    fprintf(stderr, "ncc: %u:%u: constexpr: failed to parse literal '%s'\n",
            line, col, result_str);
    exit(1);
  }

  ctx->nodes_replaced++;
  return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void ncc_register_constexpr_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "postfix_expression", xform_constexpr, "constexpr");
}
