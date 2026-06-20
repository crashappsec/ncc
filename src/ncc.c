// ncc.c — n00b C compiler: preprocess → tokenize → parse → transform → emit.
//
// Usage: ncc [flags] file.c [-- clang-flags...]
//
// NCC-specific flags (stripped before passthrough):
//   --no-ncc               Pure passthrough to underlying compiler
//   --ncc-help             Show NCC help and exit
//   --ncc-dump-tokens      Dump token stream after lexing
//   --ncc-dump-tree        Dump parse tree (pre-transform)
//   --ncc-dump-tree-raw    Dump parse tree with group wrapper nodes visible
//   --ncc-dump-output      Dump emitted C to stderr
//   --ncc-gc-stack-maps    Emit n00b GC stack-map metadata
//   --ncc-gc-typemaps      Emit n00b type-to-GC-map metadata
//   --ncc-auto-gc-roots    Auto-register TU-scope pointer globals as GC roots
//   --ncc-custom-entry     Link with ncc's generated libc-less n00b entry
//   --ncc-comptime-arg=VAL Pass VAL as a comptime_main argument
//   --ncc-no-comptime      Do not run comptime_main during this link
//   -E                     Preprocess + transform, emit to stdout
//   -c                     Compile only (pipe to clang)
//   -o FILE                Output file
//
// Environment variables:
//   NCC_COMPILER           Override underlying compiler (default: clang)
//   NCC_VERBOSE            Verbose progress messages

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef __APPLE__
#include <execinfo.h>
#elif defined(__linux__)
#include <execinfo.h>
#endif

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/typedef_walk.h"
#include "parse/symbol_populate.h"
#include "parse/type_infer.h"
#include "parse/nullability.h"
#include "parse/nodiscard.h"
#include "parse/c_tokenizer.h"
#include "parse/comptime_check.h"
#include "parse/comptime_guard.h"
#include "parse/comptime_build.h"
#include "parse/comptime_meta.h"
#include "parse/crt_entry.h"
#include "parse/emit.h"
#include "parse/union_deprecation.h"
#include "xform/xform_gc_typemap.h"
#include "xform/xform_helpers.h"
#include "parse/tree_dump.h"
#include "xform/transform.h"
#include "xform/xform_template.h"
#include "parse/symtab.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "util/platform.h"
#include "util/type_normalize.h"
#include "internal/ncc_opts.h"

static const char embedded_grammar[] = {
#embed "c_ncc.bnf"
, '\0'
};
static const size_t embedded_grammar_len = sizeof(embedded_grammar) - 1;

// Transform registration prototypes.
extern void ncc_register_rpc_xform(ncc_xform_registry_t *reg);
extern void ncc_register_generic_struct_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typeid_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typestr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typehash_xform(ncc_xform_registry_t *reg);
extern void ncc_register_once_xform(ncc_xform_registry_t *reg);
extern void ncc_register_bang_xform(ncc_xform_registry_t *reg);
extern void ncc_register_rstr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_static_image_xform(ncc_xform_registry_t *reg);
extern void ncc_register_gc_stack_maps_xform(ncc_xform_registry_t *reg);
extern void ncc_register_gc_globals_xform(ncc_xform_registry_t *reg);
extern void ncc_register_constexpr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_constexpr_paste_xform(ncc_xform_registry_t *reg);
extern void ncc_register_kargs_vargs_xform(ncc_xform_registry_t *reg);
extern void ncc_register_contracts_xform(ncc_xform_registry_t *reg);
extern void ncc_register_defer_xform(ncc_xform_registry_t *reg);
extern void ncc_register_try_xform(ncc_xform_registry_t *reg);
extern void ncc_register_nullable_xform(ncc_xform_registry_t *reg);
extern void ncc_register_option_xform(ncc_xform_registry_t *reg);
extern void ncc_register_array_literal_xform(ncc_xform_registry_t *reg);
extern void ncc_register_static_init_xform(ncc_xform_registry_t *reg);
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "lib/buffer.h"
#include "internal/parse/grammar_internal.h"

// ============================================================================
// Signal handler for crash backtrace
// ============================================================================

#define NCC_BACKTRACE_DEPTH 64

static void
crash_handler(int sig)
{
    fprintf(stderr, "\nncc: caught signal %d\n", sig);
#if defined(__APPLE__) || defined(__linux__)
    void *bt[NCC_BACKTRACE_DEPTH];
    int   n = backtrace(bt, NCC_BACKTRACE_DEPTH);
    backtrace_symbols_fd(bt, n, fileno(stderr));
#endif
    _Exit(1);
}

static void
signal_setup(void)
{
#if defined(__APPLE__) || defined(__linux__)
    static char     altstack[65536];
    static stack_t  ss = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
    struct sigaction sa = {.sa_handler = crash_handler, .sa_flags = SA_ONSTACK};

    sigaltstack(&ss, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
#else
    (void)crash_handler;
#endif
}

// ============================================================================
// Verbose logging
// ============================================================================

static bool verbose = false;

#define ncc_verbose(fmt, ...) \
    do { if (verbose) fprintf(stderr, "ncc: " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while (0)

static bool
env_var_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0") != 0;
}

static void
ncc_verbose_args(const char *prefix, const char **argv, int argc)
{
    if (!verbose || !prefix || !argv || argc <= 0) {
        return;
    }

    fprintf(stderr, "ncc: %s:", prefix);
    for (int i = 0; i < argc; i++) {
        fprintf(stderr, " %s", argv[i]);
    }
    fputc('\n', stderr);
}

static void
forward_process_output(const char *label, const char *data, size_t len,
                       FILE *stream)
{
    if (!data || len == 0 || !stream) {
        return;
    }

    if (verbose && label && label[0]) {
        fprintf(stderr, "ncc: %s follows\n", label);
    }

    fwrite(data, 1, len, stream);
}

static bool
token_text_is(ncc_token_info_t *tok, const char *text)
{
    if (!tok || !text || !ncc_option_is_set(tok->value)) {
        return false;
    }

    ncc_string_t value = ncc_option_get(tok->value);
    size_t       len   = strlen(text);
    return value.data && value.u8_bytes == len
        && memcmp(value.data, text, len) == 0;
}

static bool
array_literal_hint_context(ncc_token_stream_t *ts, int32_t index)
{
    if (index == 0) {
        return true;
    }

    ncc_token_info_t *prev = ts->tokens[index - 1];
    return token_text_is(prev, "{") || token_text_is(prev, ";")
        || token_text_is(prev, "(") || token_text_is(prev, "=")
        || token_text_is(prev, ",") || token_text_is(prev, "return");
}

static void
maybe_print_array_literal_parse_hint(ncc_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    for (int32_t i = 0; i < ts->token_count; i++) {
        ncc_token_info_t *tok = ts->tokens[i];
        if (!token_text_is(tok, "[")) {
            continue;
        }

        if (i + 1 < ts->token_count && token_text_is(ts->tokens[i + 1],
                                                     "[")) {
            continue;
        }

        if (!array_literal_hint_context(ts, i)) {
            continue;
        }

        fprintf(stderr,
                "ncc: hint: '[' at line %u, col %u looks like an array "
                "literal, but array literals are currently supported only "
                "as ncc_array_t(T) or n00b_array_t(T) declaration "
                "initializers; block-scope declarations must be const\n",
                tok ? tok->line : 0, tok ? tok->column : 0);
        return;
    }
}

#include "xform/xform_data.h"

static void
free_gc_stack_roots(ncc_gc_stack_root_t *root)
{
    while (root) {
        ncc_gc_stack_root_t *next = root->next;
        ncc_free(root->function_name);
        ncc_free(root->name);
        ncc_free(root->type_text);
        ncc_free(root->address_expr);
        ncc_free(root->num_words_expr);
        ncc_free(root);
        root = next;
    }
}

static void
add_clang_arg(ncc_opts_t *opts, const char *arg)
{
    if (opts->n_clang_args >= opts->clang_args_cap) {
        opts->clang_args_cap = opts->clang_args_cap ? opts->clang_args_cap * 2 : 32;
        opts->clang_args     = ncc_realloc(opts->clang_args,
                                           sizeof(char *) * (size_t)opts->clang_args_cap);
    }
    opts->clang_args[opts->n_clang_args++] = arg;
}

static void
add_comptime_arg(ncc_opts_t *opts, const char *arg)
{
    if (opts->n_comptime_args >= opts->comptime_args_cap) {
        opts->comptime_args_cap = opts->comptime_args_cap
                                      ? opts->comptime_args_cap * 2
                                      : 4;
        opts->comptime_args = ncc_realloc(
            opts->comptime_args,
            sizeof(char *) * (size_t)opts->comptime_args_cap);
    }
    opts->comptime_args[opts->n_comptime_args++] = arg;
}

static void
add_gcmap_include(ncc_opts_t *opts, const char *dir)
{
    if (opts->n_gcmap_includes >= opts->gcmap_includes_cap) {
        opts->gcmap_includes_cap = opts->gcmap_includes_cap
                                       ? opts->gcmap_includes_cap * 2
                                       : 4;
        opts->gcmap_includes = ncc_realloc(
            opts->gcmap_includes,
            sizeof(char *) * (size_t)opts->gcmap_includes_cap);
    }
    opts->gcmap_includes[opts->n_gcmap_includes++] = dir;
}

// Forward declaration — defined below.
static char *get_exe_path(void);
static bool ncc_arg_is_ncc_only_with_value(const char *arg);
static int custom_entry_link_passthrough(const ncc_opts_t *opts, int argc,
                                         const char **orig_argv);

static bool
path_is_sep(char c)
{
    return c == '/' || c == '\\';
}

static const char *
path_basename(const char *path)
{
    const char *base = path;

    for (const char *p = path; p && *p; p++) {
        if (path_is_sep(*p)) {
            base = p + 1;
        }
    }

    return base;
}

#ifdef _WIN32
static bool
ascii_streq_ignore_case(const char *lhs, const char *rhs)
{
    while (*lhs && *rhs) {
        if (tolower((unsigned char)*lhs) != tolower((unsigned char)*rhs)) {
            return false;
        }
        lhs++;
        rhs++;
    }

    return *lhs == '\0' && *rhs == '\0';
}
#endif

static char *
normalize_cpp_path(const char *path)
{
    size_t len = strlen(path);
    char  *out = ncc_alloc_size(1, len + 1);

    memcpy(out, path, len + 1);

    for (size_t i = 0; i < len; i++) {
        if (out[i] == '\\') {
            out[i] = '/';
        }
    }

    return out;
}

static void
print_process_message(const char *prefix, const char *detail)
{
    if (!detail || !detail[0]) {
        fprintf(stderr, "ncc: %s\n", prefix);
        return;
    }

    fprintf(stderr, "ncc: %s: %s", prefix, detail);
    if (detail[strlen(detail) - 1] != '\n') {
        fputc('\n', stderr);
    }
}

// ============================================================================
// Self-recursion guard: detect if a path resolves to our own executable
// ============================================================================

static bool
is_ncc_path(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

    // Quick basename check: if the bare name is "ncc", it's us.
    const char *base = path_basename(path);
    if (strcmp(base, "ncc") == 0 || strcmp(base, "ncc.exe") == 0
#ifdef _WIN32
        || ascii_streq_ignore_case(base, "ncc")
        || ascii_streq_ignore_case(base, "ncc.exe")
#endif
    ) {
        return true;
    }

    char *our_exe = get_exe_path();
    if (!our_exe) {
        return false;
    }

    bool same = ncc_platform_path_eq(path, our_exe);
    ncc_free(our_exe);
    return same;
}

// ============================================================================
// Argument parsing (simple hand-rolled, no commander dependency)
// ============================================================================

static void
parse_argv(ncc_opts_t *opts, int argc, const char **argv)
{
    memset(opts, 0, sizeof(*opts));

    verbose = env_var_enabled("NCC_VERBOSE");

#ifdef NCC_GC_STACK_MAPS_DEFAULT
    opts->gc_stack_maps = true;
#endif

#ifdef NCC_AUTO_GC_ROOTS_DEFAULT
    opts->auto_gc_roots = true;
#endif
    opts->gc_typemaps = true;
    opts->gcmap_prelink = false;
    opts->gcmap_emit_out = nullptr;

    opts->compiler = getenv("NCC_COMPILER");
    if (opts->compiler) {
        ncc_verbose("using NCC_COMPILER=%s", opts->compiler);
    }
    if (!opts->compiler) {
        const char *cc_env = getenv("CC");
        if (cc_env && !is_ncc_path(cc_env)) {
            opts->compiler = cc_env;
            ncc_verbose("using CC=%s as underlying compiler", cc_env);
        }
        else if (cc_env) {
            ncc_verbose("ignoring CC=%s because it resolves to ncc", cc_env);
        }
    }
#ifdef NCC_DEFAULT_CC
    if (!opts->compiler) {
        opts->compiler = NCC_DEFAULT_CC;
        ncc_verbose("using build-time default compiler=%s", opts->compiler);
    }
#endif
    if (opts->compiler && is_ncc_path(opts->compiler)) {
        ncc_verbose("ignoring compiler=%s because it resolves to ncc",
                    opts->compiler);
        opts->compiler = nullptr;
    }
    if (!opts->compiler) {
        opts->compiler = "clang";
        ncc_verbose("using fallback compiler=%s", opts->compiler);
    }

    // Init constexpr headers from env var (flag overrides later).
    const char *ce_env = getenv("NCC_CONSTEXPR_HEADERS");
    if (ce_env && ce_env[0]) {
        opts->constexpr_headers = ce_env;
    }

    // Init grammar file from env var (flag overrides later).
    const char *grammar_env = getenv("NCC_GRAMMAR");
    if (grammar_env && grammar_env[0]) {
        opts->grammar_file = grammar_env;
    }

    bool after_dashdash = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (after_dashdash) {
            add_clang_arg(opts, arg);
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            after_dashdash = true;
            continue;
        }

        // NCC-specific flags.
        if (strcmp(arg, "--no-ncc") == 0) {
            opts->no_ncc = true;
            continue;
        }
        if (strcmp(arg, "--ncc-help") == 0) {
            opts->ncc_help = true;
            continue;
        }
        if (strcmp(arg, "--ncc-dump-tokens") == 0) {
            opts->dump_tokens = true;
            continue;
        }
        if (strcmp(arg, "--ncc-dump-tree") == 0) {
            opts->dump_tree = true;
            continue;
        }
        if (strcmp(arg, "--ncc-dump-tree-raw") == 0) {
            opts->dump_tree     = true;
            opts->dump_tree_raw = true;
            continue;
        }
        if (strcmp(arg, "--ncc-dump-output") == 0) {
            opts->dump_output = true;
            continue;
        }
        if (strcmp(arg, "--ncc-gc-stack-maps") == 0) {
            opts->gc_stack_maps = true;
            continue;
        }
        if (strcmp(arg, "--ncc-gc-stack-maps-relaxed") == 0) {
            opts->gc_stack_maps         = true;
            opts->gc_stack_maps_relaxed = true;
            continue;
        }
        if (strcmp(arg, "--ncc-no-gc-stack-maps") == 0) {
            opts->gc_stack_maps         = false;
            opts->gc_stack_maps_relaxed = false;
            continue;
        }
        if (strcmp(arg, "--ncc-gc-typemaps") == 0) {
            opts->gc_typemaps = true;
            continue;
        }
        if (strcmp(arg, "--ncc-no-gc-typemaps") == 0) {
            opts->gc_typemaps = false;
            continue;
        }
        if (strcmp(arg, "--ncc-auto-gc-roots") == 0) {
            opts->auto_gc_roots = true;
            continue;
        }
        if (strcmp(arg, "--ncc-no-auto-gc-roots") == 0) {
            opts->auto_gc_roots = false;
            continue;
        }
        if (strcmp(arg, "--ncc-gcmap-prelink") == 0) {
            opts->gcmap_prelink = true;
            continue;
        }
        if (strcmp(arg, "--ncc-no-gcmap-prelink") == 0) {
            opts->gcmap_prelink = false;
            continue;
        }
        if (strncmp(arg, "--ncc-gcmap-include=", 20) == 0) {
            add_gcmap_include(opts, arg + 20);
            continue;
        }
        if (strncmp(arg, "--ncc-gcmap-emit=", 17) == 0) {
            opts->gcmap_emit_out = arg + 17;
            // Implies the aggregation machinery; mark prelink so the orchestrator
            // path is consistent (the emit mode does its own collection below).
            opts->gcmap_prelink = true;
            continue;
        }
        if (strcmp(arg, "--ncc-allow-unions") == 0) {
            opts->allow_unions = true;
            continue;
        }
        if (strcmp(arg, "--ncc-error-on-union") == 0) {
            opts->error_on_union = true;
            continue;
        }
        if (strcmp(arg, "--ncc-custom-entry") == 0) {
            opts->custom_entry = true;
            continue;
        }
        if (strncmp(arg, "--ncc-comptime-arg=", 19) == 0) {
            add_comptime_arg(opts, arg + 19);
            continue;
        }
        if (strcmp(arg, "--ncc-no-comptime") == 0) {
            opts->no_comptime = true;
            continue;
        }
        if (strcmp(arg, "--ncc-constexpr-include") == 0) {
            if (i + 1 < argc) {
                opts->constexpr_headers = argv[++i];
            }
            continue;
        }
        if (strncmp(arg, "--ncc-constexpr-include=", 23) == 0) {
            opts->constexpr_headers = arg + 23;
            continue;
        }
        if (strcmp(arg, "--ncc-grammar") == 0) {
            if (i + 1 < argc) {
                opts->grammar_file = argv[++i];
            }
            continue;
        }
        if (strncmp(arg, "--ncc-grammar=", 14) == 0) {
            opts->grammar_file = arg + 14;
            continue;
        }
        if (strncmp(arg, "--ncc-rstr-template-styled=", 27) == 0) {
            opts->rstr_template_styled = arg + 27;
            continue;
        }
        if (strncmp(arg, "--ncc-rstr-template-plain=", 26) == 0) {
            opts->rstr_template_plain = arg + 26;
            continue;
        }
        {
            static const char prefix[] =
                "--ncc-rstr-static-ref-template-styled=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_static_ref_template_styled =
                    arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] =
                "--ncc-rstr-static-ref-template-plain=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_static_ref_template_plain =
                    arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] =
                "--ncc-rstr-static-ref-expr-styled=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_static_ref_expr_styled =
                    arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] =
                "--ncc-rstr-static-ref-expr-plain=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_static_ref_expr_plain = arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] =
                "--ncc-array-literal-data-template=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->array_literal_data_template =
                    arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] = "--ncc-array-literal-data-expr=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->array_literal_data_expr = arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] = "--ncc-static-object-entry-attr=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->static_object_entry_attr =
                    arg + sizeof(prefix) - 1;
                continue;
            }
        }
        if (strcmp(arg, "--ncc-static-identity-generate-namespace") == 0) {
            opts->static_identity_generate_namespace = true;
            continue;
        }
        if (strncmp(arg, "--ncc-vargs-type=", 17) == 0) {
            opts->vargs_type = arg + 17;
            continue;
        }
        if (strncmp(arg, "--ncc-once-prefix=", 18) == 0) {
            opts->once_prefix = arg + 18;
            continue;
        }
        if (strncmp(arg, "--ncc-rstr-string-type=", 23) == 0) {
            opts->rstr_string_type = arg + 23;
            continue;
        }
        {
            static const char prefix[] = "--ncc-rstr-text-style-type=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_text_style_type = arg + sizeof(prefix) - 1;
                continue;
            }
        }
        {
            static const char prefix[] = "--ncc-rstr-style-record-type=";
            if (strncmp(arg, prefix, sizeof(prefix) - 1) == 0) {
                opts->rstr_style_record_type = arg + sizeof(prefix) - 1;
                continue;
            }
        }

        // Standard compiler flags we track.
        if (strcmp(arg, "-E") == 0) {
            opts->has_E = true;
            continue;
        }
        if (strcmp(arg, "-c") == 0) {
            opts->has_c = true;
            add_clang_arg(opts, arg);
            continue;
        }
        if (strcmp(arg, "-S") == 0) {
            opts->has_S = true;
            add_clang_arg(opts, arg);
            continue;
        }
        if (strcmp(arg, "-fsyntax-only") == 0) {
            opts->has_fsyntax_only = true;
            add_clang_arg(opts, arg);
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 < argc) {
                opts->output_file = argv[++i];
                add_clang_arg(opts, "-o");
                add_clang_arg(opts, opts->output_file);
            }
            continue;
        }
        if (strncmp(arg, "-o", 2) == 0 && arg[2] != '\0') {
            opts->output_file = arg + 2;
            add_clang_arg(opts, arg);
            continue;
        }
        if (strncmp(arg, "-std=", 5) == 0) {
            const char *std = arg + 5;
            if (strcmp(std, "c23") != 0 && strcmp(std, "gnu23") != 0
                && strcmp(std, "c2y") != 0 && strcmp(std, "gnu2y") != 0
                && strcmp(std, "c2x") != 0 && strcmp(std, "gnu2x") != 0) {
                // Non-C23 standard — force passthrough (no transforms).
                opts->no_ncc = true;
            }
            opts->has_std = true;
            add_clang_arg(opts, arg);
            continue;
        }

        // Source file: non-flag argument that looks like a C file.
        if (arg[0] != '-' && !opts->input_file) {
            const char *ext = strrchr(arg, '.');
            if (ext && (strcmp(ext, ".c") == 0 || strcmp(ext, ".nc") == 0
                        || strcmp(ext, ".h") == 0)) {
                opts->input_file = arg;
                continue;
            }
        }

        // Dependency flags: track them for separate dep generation but
        // DON'T pass to main compilation (we pipe to stdin).
        if (strcmp(arg, "-MD") == 0) {
            opts->has_dep_flags = true;
            continue;
        }
        if (strcmp(arg, "-MMD") == 0) {
            opts->has_dep_flags = true;
            opts->dep_mmmd      = true;
            continue;
        }
        if (strcmp(arg, "-M") == 0 || strcmp(arg, "-MM") == 0) {
            opts->has_dep_only = true;
            add_clang_arg(opts, arg);
            continue;
        }
        if (strcmp(arg, "-MF") == 0 && i + 1 < argc) {
            opts->dep_mf = argv[++i];
            continue;
        }
        if (strcmp(arg, "-MQ") == 0 && i + 1 < argc) {
            opts->dep_mq = argv[++i];
            continue;
        }
        if (strcmp(arg, "-MT") == 0 && i + 1 < argc) {
            opts->dep_mt = argv[++i];
            continue;
        }

        // Strip -save-temps (conflicts with stdin piping).
        if (strcmp(arg, "-save-temps") == 0
            || strncmp(arg, "-save-temps=", 12) == 0) {
            continue;
        }

        // Everything else is a clang passthrough flag.
        add_clang_arg(opts, arg);

        // Flags that take a following argument.
        if ((strcmp(arg, "-I") == 0 || strcmp(arg, "-D") == 0
             || strcmp(arg, "-U") == 0 || strcmp(arg, "-include") == 0
             || strcmp(arg, "-isystem") == 0 || strcmp(arg, "-x") == 0)
            && i + 1 < argc) {
            add_clang_arg(opts, argv[++i]);
        }
        if (strcmp(arg, "-std") == 0 && i + 1 < argc) {
            const char *std = argv[++i];
            if (strcmp(std, "c23") != 0 && strcmp(std, "gnu23") != 0
                && strcmp(std, "c2y") != 0 && strcmp(std, "gnu2y") != 0
                && strcmp(std, "c2x") != 0 && strcmp(std, "gnu2x") != 0) {
                opts->no_ncc = true;
            }
            opts->has_std = true;
            add_clang_arg(opts, "-std");
            add_clang_arg(opts, std);
        }
    }

    ncc_verbose("options: input=%s output=%s mode=%s compiler=%s clang_args=%d",
                opts->input_file ? opts->input_file : "(none)",
                opts->output_file ? opts->output_file : "(default)",
                opts->no_ncc ? "passthrough"
                             : opts->has_E ? "emit-c"
                                           : opts->has_c ? "compile-only"
                                                         : opts->has_S ? "assemble-output"
                                                                       : opts->has_fsyntax_only ? "syntax-only"
                                                                                                 : opts->has_dep_only ? "dependency-only"
                                                                                                                       : "compile-and-link",
                opts->compiler, opts->n_clang_args);
    ncc_verbose("flags: -E=%d -c=%d -S=%d syntax_only=%d dep_only=%d std=%d dep=%d dump_tokens=%d dump_tree=%d dump_output=%d gc_stack_maps=%d gc_typemaps=%d auto_gc_roots=%d custom_entry=%d no_comptime=%d comptime_args=%d",
                opts->has_E, opts->has_c, opts->has_S,
                opts->has_fsyntax_only, opts->has_dep_only,
                opts->has_std, opts->has_dep_flags,
                opts->dump_tokens, opts->dump_tree, opts->dump_output,
                opts->gc_stack_maps, opts->gc_typemaps, opts->auto_gc_roots,
                opts->custom_entry, opts->no_comptime,
                opts->n_comptime_args);
    if (opts->constexpr_headers) {
        ncc_verbose("constexpr headers=%s", opts->constexpr_headers);
    }
    if (opts->grammar_file) {
        ncc_verbose("grammar override=%s", opts->grammar_file);
    }
    if (opts->n_clang_args > 0) {
        ncc_verbose_args("clang passthrough args", opts->clang_args,
                         opts->n_clang_args);
    }
}

// ============================================================================
// Help
// ============================================================================

static void
print_help(void)
{
    fprintf(stderr,
        "ncc — n00b C compiler (source-to-source transformer)\n"
        "\n"
        "Usage: ncc [flags] file.c [-- clang-flags...]\n"
        "\n"
        "NCC-specific flags:\n"
        "  --no-ncc             Pure passthrough to underlying compiler\n"
        "  --ncc-help           Show this help\n"
        "  --ncc-dump-tokens    Dump token stream after lexing\n"
        "  --ncc-dump-tree      Dump parse tree (pre-transform)\n"
        "  --ncc-dump-tree-raw  Dump parse tree showing group wrapper nodes\n"
        "  --ncc-dump-output    Dump emitted C to stderr\n"
        "  --ncc-gc-stack-maps  Emit n00b GC stack-map metadata\n"
        "  --ncc-gc-stack-maps-relaxed\n"
        "                       Emit n00b GC stack maps, skipping unsupported roots\n"
        "  --ncc-no-gc-stack-maps\n"
        "                       Disable n00b GC stack-map metadata\n"
        "  --ncc-gc-typemaps    Emit n00b type-to-GC-map metadata\n"
        "  --ncc-no-gc-typemaps\n"
        "                       Disable n00b type-to-GC-map metadata\n"
        "  --ncc-auto-gc-roots  Auto-register TU-scope pointer-bearing globals\n"
        "                       as libn00b GC roots\n"
        "  --ncc-no-auto-gc-roots\n"
        "                       Disable auto-registration of TU-scope GC roots\n"
        "  --ncc-allow-unions   Suppress the traditional-union deprecation\n"
        "                       warning (e.g. when not targeting n00b)\n"
        "  --ncc-error-on-union Make the traditional-union deprecation a hard\n"
        "                       error (opt in ahead of the future default)\n"
        "  --ncc-custom-entry   Link with ncc's generated libc-less n00b entry\n"
        "  --ncc-comptime-arg=VAL\n"
        "                       Pass VAL as an argv element to comptime_main\n"
        "  --ncc-no-comptime    Skip link-time comptime execution\n"
        "  --ncc-constexpr-include HDRS\n"
        "                       Comma-separated headers for constexpr eval\n"
        "                       (e.g. '<myheader.h>,\"local.h\"')\n"
        "                       Overrides NCC_CONSTEXPR_HEADERS env var\n"
        "  --ncc-grammar FILE   Use external grammar file instead of built-in\n"
        "                       Overrides NCC_GRAMMAR env var\n"
        "  --ncc-rstr-text-style-type TYPE\n"
        "                       Override styled r-string text style type\n"
        "  --ncc-rstr-style-record-type TYPE\n"
        "                       Override styled r-string style record type\n"
        "  --ncc-rstr-static-ref-template-{plain,styled}=TMPL\n"
        "                       Override r-string declaration templates used by\n"
        "                       array literal static initializers\n"
        "  --ncc-rstr-static-ref-expr-{plain,styled}=EXPR\n"
        "                       Override r-string address expressions used by\n"
        "                       array literal static initializers\n"
        "  --ncc-array-literal-data-template=TMPL\n"
        "                       Legacy compatibility flag; ignored by\n"
        "                       migrated array literal lowering\n"
        "  --ncc-array-literal-data-expr=EXPR\n"
        "                       Legacy compatibility flag; ignored by\n"
        "                       migrated array literal lowering\n"
        "  --ncc-static-object-entry-attr=ATTR\n"
        "                       Attribute text for generated static-object\n"
        "                       descriptor section entries\n"
        "  --ncc-static-identity-generate-namespace\n"
        "                       Write .namespace.toml beside the source file\n"
        "                       when stable static identity metadata is absent\n"
        "\n"
        "Standard flags:\n"
        "  -E                   Preprocess + transform, emit C to stdout\n"
        "  -c                   Compile only (pipe transformed C to clang)\n"
        "  -o FILE              Output file\n"
        "\n"
        "Environment:\n"
        "  NCC_COMPILER         Override underlying compiler\n"
        "  CC                   Underlying compiler (if NCC_COMPILER unset)\n"
        "  NCC_VERBOSE          Verbose progress messages\n"
        "  NCC_CONSTEXPR_HEADERS\n"
        "                       Comma-separated headers for constexpr eval\n"
        "                       (overridden by --ncc-constexpr-include)\n"
        "  NCC_GRAMMAR          External grammar file path\n"
        "                       (overridden by --ncc-grammar)\n"
    );
}

// ============================================================================
// Get the absolute path to the ncc executable
// ============================================================================

static char *
get_exe_path(void)
{
    return ncc_platform_get_exe_path();
}


// ============================================================================
// Read a file into a string
// ============================================================================

static char *
read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char  *buf   = ncc_alloc_size(1, (size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread]   = '\0';
    fclose(f);

    if (out_len) {
        *out_len = nread;
    }
    return buf;
}

// ============================================================================
// Wrap system includes with #pragma ncc off/on
// ============================================================================

static char *
// Post-CPP: insert #pragma ncc off/on around system header regions.
//
// The C preprocessor emits line markers like:
//   # 1 "/usr/include/stdlib.h" 1 3 4
// Flag "3" means the text comes from a system header.  We detect
// transitions into/out-of system header regions and insert pragmas so
// the tokenizer/parser can skip those sections.
wrap_system_headers(const char *src, size_t src_len, size_t *out_len)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_ensure(buf, src_len + 65536);
    bool   in_system = false;

    const char *p   = src;
    const char *end = src + src_len;

    while (p < end) {
        // Detect CPP line markers at start of line: # <linenum> "file" [flags]
        if (*p == '#' && (p == src || p[-1] == '\n')) {
            const char *s = p + 1;

            // Skip spaces.
            while (s < end && *s == ' ') {
                s++;
            }

            // Check for a digit (line number).
            if (s < end && *s >= '0' && *s <= '9') {
                // Skip line number.
                while (s < end && *s >= '0' && *s <= '9') {
                    s++;
                }

                // Skip space before filename.
                while (s < end && *s == ' ') {
                    s++;
                }

                // Skip quoted filename.
                if (s < end && *s == '"') {
                    s++;
                    while (s < end && *s != '"') {
                        s++;
                    }
                    if (s < end) {
                        s++; // skip closing quote
                    }
                }

                // Now scan remaining flags on this line.
                bool has_flag_3 = false;
                const char *eol = s;
                while (eol < end && *eol != '\n') {
                    if (*eol == '3') {
                        has_flag_3 = true;
                    }
                    eol++;
                }

                // Include the newline.
                if (eol < end) {
                    eol++;
                }

                size_t line_len = (size_t)(eol - p);

                // Transition: entering system header.
                if (has_flag_3 && !in_system) {
                    in_system = true;
                    ncc_buffer_append(buf, "#pragma ncc off\n", 16);
                    ncc_buffer_append(buf, p, line_len);
                    p = eol;
                    continue;
                }

                // Transition: leaving system header (line marker without
                // flag 3 while we were in a system region).
                if (!has_flag_3 && in_system) {
                    in_system = false;
                    ncc_buffer_append(buf, "#pragma ncc on\n", 15);
                    ncc_buffer_append(buf, p, line_len);
                    p = eol;
                    continue;
                }

                // Same region — just copy the line marker.
                ncc_buffer_append(buf, p, line_len);
                p = eol;
                continue;
            }
        }

        // Regular character — just copy.
        ncc_buffer_putc(buf, *p++);
    }

    // If we ended inside a system region, close it.
    if (in_system) {
        ncc_buffer_append(buf, "#pragma ncc on\n", 15);
    }

    *out_len = buf->byte_len;
    return ncc_buffer_take(buf);
}

// ============================================================================
// Pre-CPP scan:
//   r"..." -> __ncc_rstr("...")
//   b"..." -> __ncc_buflit("...")
// ============================================================================

static bool
is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Rewrite ncc prefixed string literals in a source buffer.
 *
 * Scans the raw source before the C preprocessor runs. Tracks comment
 * and string context to avoid false matches. Handles adjacent ordinary
 * string concatenation (r"a" "b" -> __ncc_rstr("a" "b")).
 *
 * Returns a new buffer if any supported prefixed literals were found,
 * or the original buffer unchanged.
 */
static char *
ncc_literal_prescan(char *src, size_t len, size_t *out_len)
{
    // Quick check: does the buffer contain r" or b" at all?
    bool has_prefixed_literal = false;

    for (size_t i = 0; i + 1 < len; i++) {
        if ((src[i] == 'r' || src[i] == 'b') && src[i + 1] == '"') {
            if (i == 0 || !is_id_char(src[i - 1])) {
                has_prefixed_literal = true;
                break;
            }
        }
    }

    if (!has_prefixed_literal) {
        *out_len = len;
        return src;
    }

    // Build output buffer with replacements.
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_ensure(buf, len + 1024);

    enum {
        ST_NORMAL,
        ST_LINE_COMMENT,
        ST_BLOCK_COMMENT,
        ST_STRING,
        ST_CHAR,
    } state = ST_NORMAL;

    size_t i          = 0;
    size_t flush_from = 0;

    while (i < len) {
        switch (state) {
        case ST_NORMAL:
            if (src[i] == '/' && i + 1 < len && src[i + 1] == '/') {
                state = ST_LINE_COMMENT;
                i += 2;
                continue;
            }
            if (src[i] == '/' && i + 1 < len && src[i + 1] == '*') {
                state = ST_BLOCK_COMMENT;
                i += 2;
                continue;
            }
            if (src[i] == '"') {
                state = ST_STRING;
                i++;
                continue;
            }
            if (src[i] == '\'') {
                state = ST_CHAR;
                i++;
                continue;
            }
            // r"..." / b"..." match?
            if ((src[i] == 'r' || src[i] == 'b')
                && i + 1 < len && src[i + 1] == '"') {
                if (i == 0 || !is_id_char(src[i - 1])) {
                    char        prefix_ch = src[i]; // 'r' or 'b'
                    const char *callee    = prefix_ch == 'r'
                                              ? "__ncc_rstr("
                                              : "__ncc_buflit(";
                    // Flush everything before the prefix char.
                    if (i > flush_from) {
                        ncc_buffer_append(buf, src + flush_from, i - flush_from);
                    }
                    // Emit the internal call instead of the prefix.
                    ncc_buffer_append(buf, callee, strlen(callee));
                    i++; // skip the prefix char, leaving i at the opening '"'

                    // Collect one or more adjacent string literals as the
                    // single call argument. A continuation literal may be
                    // plain ("...") or carry the SAME prefix (r"..." after
                    // r"...", b"..." after b"..."); the continuation prefix is
                    // stripped so that
                    //     r"foo" r"bar"   parses exactly like   r"foobar"
                    // through ordinary adjacent-string concatenation inside the
                    // synthesized call. A differing prefix is left untouched so
                    // a nonsensical mix (e.g. r"..." b"...") surfaces as an
                    // error rather than silently merging.
                    bool first_seg = true;

                    for (;;) {
                        // i is at the opening '"' of a segment.
                        size_t seg_start = i;
                        i++; // skip opening "

                        while (i < len) {
                            if (src[i] == '\\' && i + 1 < len) {
                                i += 2;
                            }
                            else if (src[i] == '"') {
                                i++;
                                break;
                            }
                            else {
                                i++;
                            }
                        }

                        // Separate segments with a space so adjacent C string
                        // literals concatenate (and never glue into one token).
                        if (!first_seg) {
                            ncc_buffer_append(buf, " ", 1);
                        }
                        ncc_buffer_append(buf, src + seg_start, i - seg_start);
                        first_seg = false;

                        // Look past whitespace for an adjacent literal.
                        size_t ws = i;
                        while (ws < len
                               && (src[ws] == ' ' || src[ws] == '\t'
                                   || src[ws] == '\n' || src[ws] == '\r')) {
                            ws++;
                        }

                        if (ws < len && src[ws] == '"') {
                            i = ws; // plain continuation
                            continue;
                        }
                        if (ws + 1 < len
                            && src[ws] == prefix_ch
                            && src[ws + 1] == '"'
                            && (ws == 0 || !is_id_char(src[ws - 1]))) {
                            i = ws + 1; // strip same prefix, continue at '"'
                            continue;
                        }
                        break;
                    }

                    // Emit closing ).
                    ncc_buffer_append(buf, ")", 1);
                    flush_from = i;
                    continue;
                }
            }
            i++;
            break;

        case ST_LINE_COMMENT:
            if (src[i] == '\n') {
                state = ST_NORMAL;
            }
            i++;
            break;

        case ST_BLOCK_COMMENT:
            if (src[i] == '*' && i + 1 < len && src[i + 1] == '/') {
                state = ST_NORMAL;
                i += 2;
            }
            else {
                i++;
            }
            break;

        case ST_STRING:
            if (src[i] == '\\' && i + 1 < len) {
                i += 2;
            }
            else if (src[i] == '"') {
                state = ST_NORMAL;
                i++;
            }
            else {
                i++;
            }
            break;

        case ST_CHAR:
            if (src[i] == '\\' && i + 1 < len) {
                i += 2;
            }
            else if (src[i] == '\'') {
                state = ST_NORMAL;
                i++;
            }
            else {
                i++;
            }
            break;
        }
    }

    // Flush remaining bytes.
    if (flush_from < len) {
        ncc_buffer_append(buf, src + flush_from, len - flush_from);
    }

    *out_len = buf->byte_len;

    ncc_free(src);
    return ncc_buffer_take(buf);
}

// ============================================================================
// Run clang -E and capture output
// ============================================================================

static char *
run_preprocessor(const ncc_opts_t *opts, size_t *out_len)
{
    const char  *input_file = opts->input_file;
    const char  *compiler   = opts->compiler;
    const char **clang_args = opts->clang_args;
    int          n_clang_args = opts->n_clang_args;

    // Read the source file, wrap system includes, write to temp file.
    size_t raw_len = 0;
    char  *raw_src = read_file(input_file, &raw_len);

    if (!raw_src) {
        fprintf(stderr, "ncc: cannot read %s: %s\n", input_file,
                strerror(errno));
        return nullptr;
    }

    ncc_verbose("preprocess: read %zu bytes from %s", raw_len, input_file);

    // Prescan: rewrite ncc prefixed literals before CPP.
    // This must happen on the raw source before preprocessing.
    char  *wrapped     = ncc_literal_prescan(raw_src, raw_len, &raw_len);
    size_t wrapped_len = raw_len;

    // Prepend a line marker so CPP attributes output to the original file.
    // Normalize to forward slashes so the preprocessor sees a portable path.
    char         *marker_path = normalize_cpp_path(input_file);
    ncc_buffer_t *marker_buf  = ncc_buffer_empty();
    ncc_buffer_printf(marker_buf, "# 1 \"%s\"\n", marker_path);
    ncc_buffer_append(marker_buf, wrapped, wrapped_len);

    size_t total_len   = marker_buf->byte_len;
    char  *with_marker = ncc_buffer_take(marker_buf);

    ncc_free(marker_path);
    ncc_free(wrapped);
    wrapped     = with_marker;
    wrapped_len = total_len;
    ncc_verbose("preprocess: source after prescan/marker is %zu bytes",
                wrapped_len);

    // Write wrapped source into a private temp directory.
    ncc_temp_workspace_t pp_workspace = {0};
    char *tmp_err  = nullptr;
    char *tmp_path = nullptr;

    if (!ncc_temp_workspace_create(&pp_workspace, "ncc_pp_", &tmp_err)) {
        print_process_message("failed to create preprocessor temp path",
                              tmp_err);
        ncc_free(tmp_err);
        ncc_free(wrapped);
        return nullptr;
    }

    tmp_path = ncc_temp_workspace_join(&pp_workspace, "input.c");
    if (!tmp_path) {
        fprintf(stderr, "ncc: failed to create preprocessor input path\n");
        ncc_free(wrapped);
        ncc_temp_workspace_cleanup(&pp_workspace);
        return nullptr;
    }

    ncc_verbose("preprocess temp dir=%s",
                ncc_temp_workspace_path(&pp_workspace));
    ncc_verbose("preprocess temp input=%s", tmp_path);

    char *write_err = nullptr;
    if (!ncc_platform_write_file(tmp_path, wrapped, wrapped_len, &write_err)) {
        print_process_message("write to preprocessor temp file failed",
                              write_err);
        ncc_free(write_err);
        ncc_free(wrapped);
        ncc_free(tmp_path);
        ncc_temp_workspace_cleanup(&pp_workspace);
        return nullptr;
    }

    ncc_free(wrapped);
    ncc_verbose("preprocess: wrote %zu bytes to %s", wrapped_len, tmp_path);

    // Extract source directory for -I so relative includes still resolve.
    char       *src_dir     = ncc_platform_dirname(input_file);
    const char *include_dir = src_dir;
    if (include_dir) {
        ncc_verbose("preprocess include dir=%s", include_dir);
    }

    // Build argv: compiler -E -std=gnu23 -fno-blocks [-I src_dir]
    //             [filtered clang_args...] tmp_path
    // Filter out -c, -o (and its argument) from preprocessor args.
    int extra = include_dir ? 2 : 0;
    int argc  = 6 + extra + n_clang_args; // extra room for -std/-fno-blocks

    const char **argv = ncc_alloc_array(const char *, (size_t)(argc + 1));
    int          ai   = 0;

    argv[ai++] = compiler;
    argv[ai++] = "-E";

    // Ensure C23 mode for the preprocessor (needed for features like
    // single-argument va_start).
    if (!opts->has_std) {
        argv[ai++] = "-std=gnu23";
    }

    // Disable Apple block syntax in system headers.
    argv[ai++] = "-fno-blocks";

    if (include_dir) {
        argv[ai++] = "-I";
        argv[ai++] = include_dir;
    }

    for (int i = 0; i < n_clang_args; i++) {
        // Skip -c (compile-only flag, irrelevant for preprocessing).
        if (strcmp(clang_args[i], "-c") == 0) {
            continue;
        }
        // Skip -o and its argument (output file, irrelevant for preprocessing).
        if (strcmp(clang_args[i], "-o") == 0) {
            i++; // skip the argument too
            continue;
        }
        if (strncmp(clang_args[i], "-o", 2) == 0 && clang_args[i][2] != '\0') {
            continue;
        }
        argv[ai++] = clang_args[i];
    }

    argv[ai++] = tmp_path;
    argv[ai]   = nullptr;

    ncc_verbose_args("preprocessor argv", argv, ai);

    ncc_process_spec_t   spec = {
        .program        = compiler,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch preprocessor",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(src_dir);
        ncc_free(tmp_path);
        ncc_temp_workspace_cleanup(&pp_workspace);
        return nullptr;
    }

    forward_process_output("preprocessor stderr", proc.stderr_data,
                           proc.stderr_len, stderr);

    ncc_free(argv);
    ncc_free(src_dir);
    ncc_verbose("preprocessor exit code=%d", proc.exit_code);

    if (proc.exit_code != 0) {
        fprintf(stderr, "ncc: preprocessor failed (exit %d)\n", proc.exit_code);
        ncc_process_result_free(&proc);
        ncc_free(tmp_path);
        ncc_temp_workspace_cleanup(&pp_workspace);
        return nullptr;
    }

    size_t len = proc.stdout_len;
    char  *buf = proc.stdout_data;
    proc.stdout_data = nullptr;
    proc.stdout_len  = 0;
    ncc_process_result_free(&proc);

    ncc_verbose("preprocess: captured %zu bytes from stdout", len);
    ncc_free(tmp_path);
    ncc_temp_workspace_cleanup(&pp_workspace);

    // Post-process: wrap system header regions with #pragma ncc off/on.
    // CPP line markers with flag 3 indicate system headers.
    size_t wrapped_pp_len = 0;
    char  *wrapped_pp     = wrap_system_headers(buf, len, &wrapped_pp_len);
    ncc_free(buf);

    // A project macro can expand to r"..." / b"..." only after CPP has run,
    // and user headers are only visible in the preprocessed stream. Run the
    // same lexer-aware rewrite once more before tokenization.
    wrapped_pp = ncc_literal_prescan(wrapped_pp, wrapped_pp_len,
                                     &wrapped_pp_len);

    if (out_len) {
        *out_len = wrapped_pp_len;
    }

    ncc_verbose("preprocess: final wrapped output is %zu bytes", wrapped_pp_len);

    return wrapped_pp;
}

// ============================================================================
// Compiler passthrough (exec clang with all original args)
// ============================================================================

static bool ncc_arg_is_ncc_only_with_value(const char *arg);

static int
compiler_passthrough(const ncc_opts_t *opts, int argc, const char **argv)
{
    ncc_verbose("passthrough to %s", opts->compiler);

    // Build argv: compiler + original args (minus ncc-specific ones).
    const char **new_argv = ncc_alloc_array(const char *, (size_t)(argc + 2));
    int          n        = 0;

    new_argv[n++] = opts->compiler;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        // Skip ncc-specific flags. If the ncc flag consumes a separate value,
        // drop that value too so clang does not see it as a stray input file.
        if (strncmp(arg, "--ncc-", 6) == 0 || strcmp(arg, "--no-ncc") == 0) {
            if (ncc_arg_is_ncc_only_with_value(arg) && i + 1 < argc) {
                i++;
            }
            continue;
        }
        new_argv[n++] = arg;
    }
    new_argv[n] = nullptr;

    ncc_verbose_args("passthrough argv", new_argv, n);

    ncc_process_spec_t   spec = {
        .program        = opts->compiler,
        .argv           = new_argv,
        .capture_stdout = verbose,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch compiler passthrough",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(new_argv);
        return 1;
    }

    forward_process_output("compiler stdout", proc.stdout_data,
                           proc.stdout_len, stdout);
    forward_process_output("compiler stderr", proc.stderr_data,
                           proc.stderr_len, stderr);

    ncc_free(new_argv);
    int rc = proc.exit_code;
    ncc_verbose("passthrough exit code=%d", rc);
    ncc_process_result_free(&proc);
    return rc;
}

static int
ncc_link_passthrough(const ncc_opts_t *opts, int argc, const char **argv)
{
    if (opts->custom_entry && !opts->has_E && !opts->has_c && !opts->has_S
        && !opts->has_fsyntax_only) {
        return custom_entry_link_passthrough(opts, argc, argv);
    }

    return compiler_passthrough(opts, argc, argv);
}

static bool
ncc_arg_is_ncc_only_with_value(const char *arg)
{
    return strcmp(arg, "--ncc-constexpr-include") == 0
        || strcmp(arg, "--ncc-grammar") == 0;
}

// ============================================================================
// Load the C grammar
// ============================================================================

static ncc_grammar_t *
load_c_grammar(const ncc_opts_t *opts)
{
    const char *bnf_data;
    size_t      bnf_len;
    char       *file_buf = nullptr;

    if (opts->grammar_file) {
        file_buf = read_file(opts->grammar_file, &bnf_len);
        if (!file_buf) {
            fprintf(stderr, "ncc: cannot read grammar file: %s\n",
                    opts->grammar_file);
            return nullptr;
        }
        bnf_data = file_buf;
        ncc_verbose("grammar from file: %s", opts->grammar_file);
    } else {
        bnf_data = embedded_grammar;
        bnf_len  = embedded_grammar_len;
        ncc_verbose("using embedded grammar");
    }

    ncc_string_t bnf_text = ncc_string_from_raw(bnf_data, (int64_t)bnf_len);
    if (file_buf) ncc_free(file_buf);

    ncc_grammar_t *g = ncc_grammar_new();
    ncc_grammar_set_error_recovery(g, false);

    ncc_string_t start = NCC_STRING_STATIC("translation_unit");
    bool          ok    = ncc_bnf_load(bnf_text, start, g);

    if (!ok) {
        fprintf(stderr, "ncc: failed to load C grammar from BNF\n");
        ncc_grammar_free(g);
        return nullptr;
    }

    ncc_verbose("grammar loaded (%zu NTs, %zu rules)",
                ncc_list_len(g->nt_list), ncc_list_len(g->rules));

    return g;
}

// ============================================================================
// Dump token stream
// ============================================================================

static void
dump_tokens(ncc_token_stream_t *ts, FILE *out)
{
    for (int32_t i = 0; i < ts->token_count; i++) {
        ncc_token_info_t *t = ts->tokens[i];
        if (!t) {
            continue;
        }

        fprintf(out, "[%d] tid=%-4d line=%-6u col=%-4u", i, t->tid,
                t->line, t->column);

        if (t->system_header) {
            fprintf(out, " system_header");
        }

        if (ncc_option_is_set(t->value)) {
            ncc_string_t v = ncc_option_get(t->value);
            if (v.data) {
                fprintf(out, " \"%.60s\"", v.data);
            }
        }
        fprintf(out, "\n");
    }
}

// ============================================================================
// Pipe transformed C to compiler
// ============================================================================

static char *
write_transformed_source_temp(ncc_temp_workspace_t *workspace,
                              const char *prefix,
                              const char *c_source,
                              size_t c_len)
{
    char *tmp_err = nullptr;

    if (!ncc_temp_workspace_create(workspace, prefix, &tmp_err)) {
        print_process_message("failed to create compiler source temp path",
                              tmp_err);
        ncc_free(tmp_err);
        return nullptr;
    }

    char *path = ncc_temp_workspace_join(workspace, "input.c");
    if (!path) {
        fprintf(stderr, "ncc: failed to create compiler source path\n");
        ncc_temp_workspace_cleanup(workspace);
        return nullptr;
    }

    char *write_err = nullptr;
    if (!ncc_platform_write_file(path, c_source, c_len, &write_err)) {
        print_process_message("write to compiler source temp file failed",
                              write_err);
        ncc_free(write_err);
        ncc_free(path);
        ncc_temp_workspace_cleanup(workspace);
        return nullptr;
    }

    return path;
}

static bool
is_linker_input(const char *arg)
{
    size_t len = strlen(arg);

    if (len >= 2 && arg[0] != '-') {
        const char *dot = strrchr(arg, '.');
        if (dot) {
            if (strcmp(dot, ".a") == 0 || strcmp(dot, ".o") == 0
                || strcmp(dot, ".so") == 0 || strcmp(dot, ".dylib") == 0) {
                return true;
            }
        }
    }

    return false;
}

static bool
is_metadata_linker_input(const char *arg)
{
    if (!arg || arg[0] == '-') {
        return false;
    }

    const char *dot = strrchr(arg, '.');
    return dot && (strcmp(dot, ".o") == 0 || strcmp(dot, ".a") == 0);
}

typedef struct {
    const char **user_inputs;
    int          n_user_inputs;
    const char **metadata_inputs;
    int          n_metadata_inputs;
    const char **ordered_link_args;
    int          n_ordered_link_args;
    const char **link_args;
    int          n_link_args;
} ncc_link_inputs_t;

static void
ncc_link_inputs_free(ncc_link_inputs_t *inputs)
{
    if (!inputs) {
        return;
    }

    ncc_free(inputs->user_inputs);
    ncc_free(inputs->metadata_inputs);
    ncc_free(inputs->ordered_link_args);
    ncc_free(inputs->link_args);
    memset(inputs, 0, sizeof(*inputs));
}

static void
ncc_push_const_arg(const char ***items, int *n_items, const char *arg)
{
    *items = ncc_realloc(*items, sizeof(char *) * (size_t)(*n_items + 1));
    (*items)[(*n_items)++] = arg;
}

static void
ncc_insert_const_arg(const char ***items, int *n_items, int index,
                     const char *arg)
{
    if (index < 0) {
        index = 0;
    }
    if (index > *n_items) {
        index = *n_items;
    }

    *items = ncc_realloc(*items, sizeof(char *) * (size_t)(*n_items + 1));
    memmove((void *)(*items + index + 1), *items + index,
            sizeof(char *) * (size_t)(*n_items - index));
    (*items)[index] = arg;
    (*n_items)++;
}

static void
ncc_collect_link_input_arg(ncc_link_inputs_t *inputs, const char *arg)
{
    if (is_linker_input(arg)) {
        ncc_push_const_arg(&inputs->user_inputs, &inputs->n_user_inputs, arg);
    }
    if (is_metadata_linker_input(arg)) {
        ncc_push_const_arg(&inputs->metadata_inputs,
                           &inputs->n_metadata_inputs, arg);
    }
}

static void
ncc_collect_link_arg(ncc_link_inputs_t *inputs, const char *arg)
{
    ncc_push_const_arg(&inputs->link_args, &inputs->n_link_args, arg);
}

static void
ncc_collect_ordered_link_arg(ncc_link_inputs_t *inputs, const char *arg)
{
    ncc_push_const_arg(&inputs->ordered_link_args,
                       &inputs->n_ordered_link_args, arg);
}

static bool ncc_replay_link_arg_takes_value(const char *arg);
static bool ncc_replay_link_arg_is_single(const char *arg);

static void
ncc_collect_link_inputs_from_clang_args(const ncc_opts_t *opts,
                                        ncc_link_inputs_t *inputs)
{
    memset(inputs, 0, sizeof(*inputs));

    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (strcmp(arg, "-o") == 0) {
            i++;
            continue;
        }
        if (strncmp(arg, "-o", 2) == 0 && arg[2] != '\0') {
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "-S") == 0
            || strcmp(arg, "-E") == 0 || strcmp(arg, "-fsyntax-only") == 0) {
            continue;
        }

        if (is_linker_input(arg)) {
            ncc_collect_link_input_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            continue;
        }

        if (ncc_replay_link_arg_takes_value(arg)) {
            ncc_collect_link_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            if (i + 1 < opts->n_clang_args) {
                ncc_collect_link_arg(inputs, opts->clang_args[++i]);
                ncc_collect_ordered_link_arg(inputs, opts->clang_args[i]);
            }
            continue;
        }
        if (ncc_replay_link_arg_is_single(arg)) {
            ncc_collect_link_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            continue;
        }
    }
}

static void
ncc_collect_link_inputs_from_argv(const ncc_opts_t *opts, int argc,
                                  const char **orig_argv,
                                  ncc_link_inputs_t *inputs)
{
    (void)opts;
    memset(inputs, 0, sizeof(*inputs));

    for (int i = 1; i < argc; i++) {
        const char *arg = orig_argv[i];

        if (strncmp(arg, "--ncc-", 6) == 0 || strcmp(arg, "--no-ncc") == 0) {
            if (ncc_arg_is_ncc_only_with_value(arg) && i + 1 < argc) {
                i++;
            }
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            i++;
            continue;
        }
        if (strncmp(arg, "-o", 2) == 0 && arg[2] != '\0') {
            continue;
        }

        if (is_linker_input(arg)) {
            ncc_collect_link_input_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            continue;
        }

        if (ncc_replay_link_arg_takes_value(arg)) {
            ncc_collect_link_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            if (i + 1 < argc) {
                ncc_collect_link_arg(inputs, orig_argv[++i]);
                ncc_collect_ordered_link_arg(inputs, orig_argv[i]);
            }
            continue;
        }
        if (ncc_replay_link_arg_is_single(arg)) {
            ncc_collect_link_arg(inputs, arg);
            ncc_collect_ordered_link_arg(inputs, arg);
            continue;
        }
    }
}

static bool
ncc_read_comptime_metadata_inputs(const ncc_opts_t *opts,
                                  const char *const *inputs, int n_inputs,
                                  ncc_ct_rec_list_t *records,
                                  char **err_out)
{
    for (int i = 0; i < n_inputs; i++) {
        if (!ncc_ct_read_object(opts, inputs[i], records, err_out)) {
            return false;
        }
    }

    return true;
}

static bool
ncc_aggregate_comptime_records(const ncc_ct_rec_list_t *records,
                               ncc_ct_aggregate_t *agg, char **err_out)
{
    memset(agg, 0, sizeof(*agg));
    return ncc_ct_aggregate(records, agg, err_out);
}

static const char *
ncc_link_output_file(const ncc_opts_t *opts)
{
    return opts && opts->output_file ? opts->output_file : "a.out";
}

static int
ncc_strip_comptime_output(const ncc_opts_t *opts)
{
    char *err = nullptr;

    if (ncc_ct_strip_section(opts, ncc_link_output_file(opts), &err)) {
        ncc_free(err);
        return 0;
    }

    print_process_message("failed to strip comptime metadata", err);
    ncc_free(err);
    return 1;
}

static bool
ncc_link_arg_takes_value(const char *arg)
{
    return strcmp(arg, "-Xlinker") == 0 || strcmp(arg, "-framework") == 0
        || strcmp(arg, "-weak_framework") == 0 || strcmp(arg, "-l") == 0
        || strcmp(arg, "-L") == 0 || strcmp(arg, "-F") == 0
        || strcmp(arg, "-u") == 0 || strcmp(arg, "-rpath") == 0
        || strcmp(arg, "-install_name") == 0
        || strcmp(arg, "-compatibility_version") == 0
        || strcmp(arg, "-current_version") == 0
        || strcmp(arg, "-undefined") == 0;
}

static bool
ncc_link_arg_is_single(const char *arg)
{
    return strncmp(arg, "-Wl,", 4) == 0
        || (strncmp(arg, "-l", 2) == 0 && arg[2] != '\0')
        || (strncmp(arg, "-L", 2) == 0 && arg[2] != '\0')
        || (strncmp(arg, "-F", 2) == 0 && arg[2] != '\0')
        || strcmp(arg, "-shared") == 0 || strcmp(arg, "-static") == 0
        || strcmp(arg, "-rdynamic") == 0 || strcmp(arg, "-nostdlib") == 0
        || strcmp(arg, "-nodefaultlibs") == 0
        || strcmp(arg, "-nostartfiles") == 0 || strcmp(arg, "-pie") == 0
        || strcmp(arg, "-no-pie") == 0;
}

static bool
ncc_replay_link_arg_takes_value(const char *arg)
{
    return ncc_link_arg_takes_value(arg)
        || strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0
        || strcmp(arg, "-arch") == 0 || strcmp(arg, "-isysroot") == 0
        || strcmp(arg, "--sysroot") == 0;
}

static bool
ncc_replay_link_arg_is_single(const char *arg)
{
    return ncc_link_arg_is_single(arg)
        || strncmp(arg, "--target=", 9) == 0
        || strncmp(arg, "-target=", 8) == 0
        || strncmp(arg, "--sysroot=", 10) == 0
        || strncmp(arg, "-isysroot=", 10) == 0
        || strncmp(arg, "-mmacosx-version-min=", 21) == 0
        || strncmp(arg, "-miphoneos-version-min=", 24) == 0
        || strncmp(arg, "-mios-simulator-version-min=", 28) == 0
        || strncmp(arg, "-mtvos-version-min=", 19) == 0
        || strncmp(arg, "-mwatchos-version-min=", 22) == 0
        || strncmp(arg, "-fuse-ld=", 9) == 0
        || strcmp(arg, "-pthread") == 0;
}

static int
ncc_run_comptime_link_plan(const ncc_opts_t *opts,
                           const ncc_link_inputs_t *inputs,
                           const ncc_ct_aggregate_t *agg)
{
    const char **ct_argv = ncc_collect_comptime_argv(
        ncc_link_output_file(opts), opts->comptime_args,
        opts->n_comptime_args);
    char *err = nullptr;

    ncc_comptime_plan_t plan = {
        .user_inputs      = inputs->user_inputs,
        .n_user_inputs    = inputs->n_user_inputs,
        .output_file      = ncc_link_output_file(opts),
        .runtime_inputs   = nullptr,
        .n_runtime_inputs = 0,
        .ordered_link_args   = inputs->ordered_link_args,
        .n_ordered_link_args = inputs->n_ordered_link_args,
        .link_args        = inputs->link_args,
        .n_link_args      = inputs->n_link_args,
        .comptime_argv    = ct_argv,
        .n_comptime_argv  = opts->n_comptime_args + 1,
        .meta             = agg,
    };

    int rc = ncc_comptime_run_and_link(opts, &plan, &err);
    if (rc != 0) {
        print_process_message("comptime build failed", err);
    }

    ncc_free(err);
    ncc_free((void *)ct_argv);
    return rc;
}

static int
ncc_degrade_comptime_link_plan(const ncc_opts_t *opts,
                               const ncc_link_inputs_t *inputs,
                               const ncc_ct_aggregate_t *agg)
{
    char *err = nullptr;

    ncc_comptime_plan_t plan = {
        .user_inputs      = inputs->user_inputs,
        .n_user_inputs    = inputs->n_user_inputs,
        .output_file      = ncc_link_output_file(opts),
        .runtime_inputs   = nullptr,
        .n_runtime_inputs = 0,
        .ordered_link_args   = inputs->ordered_link_args,
        .n_ordered_link_args = inputs->n_ordered_link_args,
        .link_args        = inputs->link_args,
        .n_link_args      = inputs->n_link_args,
        .meta             = agg,
    };

    int rc = ncc_comptime_degrade_and_link(opts, &plan, &err);
    if (rc != 0) {
        print_process_message("comptime degrade build failed", err);
    }

    ncc_free(err);
    return rc;
}

static bool
ncc_compile_transformed_object(const ncc_opts_t *opts, const char *c_source,
                               size_t c_len, const char *object_path)
{
    ncc_temp_workspace_t source_workspace = {0};
    char *source_path = write_transformed_source_temp(&source_workspace,
                                                      "ncc_ct_compile_",
                                                      c_source,
                                                      c_len);

    if (!source_path) {
        return false;
    }

    int max_args = 16 + opts->n_clang_args * 2;
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args + 1);
    int ai = 0;

    argv[ai++] = opts->compiler;

    if (!opts->has_std) {
        argv[ai++] = "-std=gnu23";
    }

    argv[ai++] = "-Wno-odr";

    for (int i = 0; i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (strcmp(arg, "-o") == 0) {
            i++;
            continue;
        }
        if (strncmp(arg, "-o", 2) == 0 && arg[2] != '\0') {
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "-S") == 0
            || strcmp(arg, "-E") == 0 || strcmp(arg, "-fsyntax-only") == 0) {
            continue;
        }
        if (strcmp(arg, "-include") == 0) {
            i++;
            continue;
        }
        if (is_linker_input(arg)) {
            continue;
        }
        if (ncc_link_arg_takes_value(arg)) {
            i++;
            continue;
        }
        if (ncc_link_arg_is_single(arg)) {
            continue;
        }

        argv[ai++] = arg;
    }

    argv[ai++] = "-c";
    argv[ai++] = "-x";
    argv[ai++] = "c";
    argv[ai++] = source_path;
    argv[ai++] = "-o";
    argv[ai++] = object_path;
    argv[ai] = nullptr;

    ncc_verbose("comptime source object=%s", object_path);
    ncc_verbose_args("comptime source compiler argv", argv, ai);

    ncc_process_spec_t spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = verbose,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch comptime source compiler",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(source_path);
        ncc_temp_workspace_cleanup(&source_workspace);
        return false;
    }

    forward_process_output("comptime source compiler stdout",
                           proc.stdout_data, proc.stdout_len, stdout);
    forward_process_output("comptime source compiler stderr",
                           proc.stderr_data, proc.stderr_len, stderr);

    int rc = proc.exit_code;
    ncc_verbose("comptime source compiler exit code=%d", rc);
    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_free(source_path);
    ncc_temp_workspace_cleanup(&source_workspace);

    if (rc != 0) {
        fprintf(stderr, "ncc: comptime source object compilation failed (exit %d)\n",
                rc);
        return false;
    }

    return true;
}

static int
maybe_comptime_link_passthrough(const ncc_opts_t *opts, int argc,
                                const char **orig_argv, bool *handled)
{
    *handled = false;

    ncc_opts_t link_opts = *opts;
    if (!link_opts.input_file) {
        link_opts.input_file = "<link>";
    }

    if (!ncc_is_link_invocation(&link_opts)) {
        return 0;
    }

    ncc_link_inputs_t inputs = {0};
    ncc_ct_rec_list_t records = {0};
    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;
    int rc = 0;

    ncc_collect_link_inputs_from_argv(&link_opts, argc, orig_argv, &inputs);
    if (inputs.n_metadata_inputs == 0) {
        goto cleanup;
    }

    if (!ncc_read_comptime_metadata_inputs(&link_opts, inputs.metadata_inputs,
                                           inputs.n_metadata_inputs,
                                           &records, &err)
        || !ncc_aggregate_comptime_records(&records, &agg, &err)) {
        print_process_message("failed to read comptime metadata", err);
        rc = 1;
        *handled = true;
        goto cleanup;
    }

    if (!ncc_comptime_guard_check(&link_opts, &agg, &err)) {
        *handled = true;
        print_process_message("comptime guard failed", err);
        rc = 1;
        goto cleanup;
    }

    bool has_comptime_work = agg.has_comptime_main || agg.n_static_inits > 0;
    ncc_comptime_degrade_route_t degrade_route =
        ncc_comptime_degrade_route(&link_opts, &agg);

    if ((degrade_route.static_init_degrade
         || degrade_route.comptime_main_degrade)
        && agg.n_static_inits > 0
        && !ncc_static_init_degrade_allowed(&agg, &err)) {
        *handled = true;
        print_process_message("static-init degrade failed", err);
        rc = 1;
        goto cleanup;
    }

    if (degrade_route.comptime_main_degrade
        || degrade_route.static_init_degrade) {
        *handled = true;
        rc = ncc_degrade_comptime_link_plan(&link_opts, &inputs, &agg);
        goto cleanup;
    }

    if (link_opts.no_comptime) {
        *handled = true;
        rc = ncc_link_passthrough(&link_opts, argc, orig_argv);
        if (rc == 0) {
            rc = ncc_strip_comptime_output(&link_opts);
        }
        goto cleanup;
    }

    if (!has_comptime_work) {
        *handled = true;
        rc = ncc_link_passthrough(&link_opts, argc, orig_argv);
        if (rc == 0) {
            rc = ncc_strip_comptime_output(&link_opts);
        }
        goto cleanup;
    }

    *handled = true;
    rc = ncc_run_comptime_link_plan(&link_opts, &inputs, &agg);

cleanup:
    ncc_free(err);
    ncc_ct_aggregate_free(&agg);
    ncc_ct_rec_list_free(&records);
    ncc_link_inputs_free(&inputs);
    return rc;
}

static bool
ncc_tree_nt_is(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node) || !name) {
        return false;
    }

    ncc_string_t node_name = ncc_tree_node_value(node).name;
    return node_name.data && strcmp(node_name.data, name) == 0;
}

static ncc_parse_tree_t *
ncc_tree_child_nt(ncc_parse_tree_t *node, const char *child_name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (!child || ncc_tree_is_leaf(child)) {
            continue;
        }

        ncc_nt_node_t *nt = &ncc_tree_node_value(child);
        if (nt->group_top) {
            ncc_parse_tree_t *found = ncc_tree_child_nt(child, child_name);
            if (found) {
                return found;
            }
            continue;
        }

        if (nt->name.data && strcmp(nt->name.data, child_name) == 0) {
            return child;
        }
    }

    return nullptr;
}

static ncc_token_info_t *
ncc_declared_name_tok(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        return tok && tok->tid == NCC_TOK_IDENTIFIER ? tok : nullptr;
    }

    if (ncc_tree_nt_is(node, "parameter_type_list")
        || ncc_tree_nt_is(node, "parameter_list")) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *tok = ncc_declared_name_tok(ncc_tree_child(node, i));
        if (tok) {
            return tok;
        }
    }

    return nullptr;
}

static bool
ncc_has_function_definition_named(ncc_parse_tree_t *node, const char *name)
{
    if (!node || !name) {
        return false;
    }

    if (!ncc_tree_is_leaf(node) && ncc_tree_nt_is(node, "function_definition")) {
        ncc_parse_tree_t *declr = ncc_tree_child_nt(node, "declarator");
        ncc_token_info_t *tok   = ncc_declared_name_tok(declr);
        if (token_text_is(tok, name)) {
            return true;
        }
    }

    if (ncc_tree_is_leaf(node)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_has_function_definition_named(ncc_tree_child(node, i), name)) {
            return true;
        }
    }

    return false;
}

static bool
ncc_append_fragment(ncc_string_t *emitted, const char *fragment)
{
    if (!emitted || !emitted->data || !fragment || !fragment[0]) {
        return true;
    }

    size_t fragment_len = strlen(fragment);
    ncc_buffer_t *buf   = ncc_buffer_empty();
    ncc_buffer_ensure(buf, emitted->u8_bytes + fragment_len + 2);
    ncc_buffer_append(buf, emitted->data, emitted->u8_bytes);
    if (emitted->u8_bytes == 0
        || emitted->data[emitted->u8_bytes - 1] != '\n') {
        ncc_buffer_putc(buf, '\n');
    }
    ncc_buffer_append(buf, fragment, fragment_len);

    ncc_free(emitted->data);
    emitted->data     = ncc_buffer_take(buf);
    emitted->u8_bytes = strlen(emitted->data);
    return true;
}

static bool
ncc_subtree_has_token_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node || !text) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        return token_text_is(tok, text);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_subtree_has_token_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

static void
ncc_free_comptime_vars(ncc_ct_var_t *vars, int n_vars)
{
    if (!vars) {
        return;
    }

    for (int i = 0; i < n_vars; i++) {
        ncc_free(vars[i].name.data);
    }
    ncc_free(vars);
}

static void
ncc_free_static_inits(ncc_ct_static_init_t *static_inits, int n_static_inits)
{
    if (!static_inits) {
        return;
    }

    for (int i = 0; i < n_static_inits; i++) {
        ncc_free(static_inits[i].name.data);
    }
    ncc_free(static_inits);
}

static bool
ncc_type_spelling_is_pointer(const char *type)
{
    if (!type) {
        return false;
    }

    size_t len = strlen(type);
    while (len > 0
           && (type[len - 1] == ' ' || type[len - 1] == '\t'
               || type[len - 1] == '\n' || type[len - 1] == '\r')) {
        len--;
    }

    return len > 0 && type[len - 1] == '*';
}

static bool
ncc_type_spelling_is_root_const(const char *type)
{
    int paren_depth = 0;
    int brace_depth = 0;

    for (const char *p = type; p && *p; p++) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '(') {
            paren_depth++;
            continue;
        }
        if (ch == ')' && paren_depth > 0) {
            paren_depth--;
            continue;
        }
        if (ch == '{') {
            brace_depth++;
            continue;
        }
        if (ch == '}' && brace_depth > 0) {
            brace_depth--;
            continue;
        }
        if (paren_depth != 0 || brace_depth != 0) {
            continue;
        }
        bool token_start = p == type
                        || (!isalnum((unsigned char)p[-1]) && p[-1] != '_');
        if (token_start
            && strncmp(p, "const", 5) == 0
            && (p[5] == '\0'
                || (!isalnum((unsigned char)p[5]) && p[5] != '_'))) {
            return true;
        }
    }

    return false;
}

static bool
ncc_push_comptime_var(ncc_ct_var_t **vars, int *n_vars, int *cap,
                      ncc_sym_entry_t *entry)
{
    if (*n_vars >= *cap) {
        int new_cap = *cap ? *cap * 2 : 4;
        *vars = ncc_realloc(*vars, (size_t)new_cap * sizeof(**vars));
        *cap  = new_cap;
    }

    char *type = ncc_type_of_symbol(entry);
    if (!type) {
        fprintf(stderr,
                "ncc: unsupported comptime variable type for '%s'\n",
                entry->name.data ? entry->name.data : "<unnamed>");
        return false;
    }

    ncc_ct_var_t *out = &(*vars)[(*n_vars)++];
    out->name = ncc_string_from_raw(entry->name.data,
                                    (int64_t)entry->name.u8_bytes);
    out->typehash = ncc_type_hash_u64(type);
    out->linkage = ncc_subtree_has_token_text(entry->type_node, "static")
                       ? 0
                       : 1;
    out->flags = ncc_type_spelling_is_pointer(type)
                     ? NCC_CT_VAR_FLAG_POINTER_ROOT
                     : 0;

    ncc_free(type);
    return true;
}

static bool
ncc_collect_comptime_vars(ncc_symtab_t *symtab, ncc_ct_var_t **out_vars,
                          int *out_n_vars)
{
    *out_vars   = nullptr;
    *out_n_vars = 0;
    if (!symtab) {
        return true;
    }

    ncc_ct_var_t *vars = nullptr;
    int           n_vars = 0;
    int           cap = 0;

    for (ncc_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        for (ncc_sym_entry_t *entry = scope->first_in_scope; entry;
             entry = entry->next_in_scope) {
            if (entry->kind != NCC_SYM_VARIABLE || !entry->is_comptime
                || entry->scope_depth != 1) {
                continue;
            }
            if (!ncc_push_comptime_var(&vars, &n_vars, &cap, entry)) {
                ncc_free_comptime_vars(vars, n_vars);
                return false;
            }
        }
    }

    *out_vars   = vars;
    *out_n_vars = n_vars;
    return true;
}

static bool
ncc_push_static_init(ncc_ct_static_init_t **static_inits, int *n_static_inits,
                     int *cap, ncc_sym_entry_t *entry)
{
    if (ncc_subtree_has_token_text(entry->type_node, "static")) {
        fprintf(stderr,
                "ncc: static initializer '%s' has internal linkage; WP-005 "
                "requires external static-init roots\n",
                entry->name.data ? entry->name.data : "<unnamed>");
        return false;
    }

    if (*n_static_inits >= *cap) {
        int new_cap = *cap ? *cap * 2 : 4;
        *static_inits = ncc_realloc(
            *static_inits, (size_t)new_cap * sizeof(**static_inits));
        *cap = new_cap;
    }

    char *type = ncc_type_of_symbol(entry);
    if (!type) {
        fprintf(stderr,
                "ncc: unsupported static initializer type for '%s'\n",
                entry->name.data ? entry->name.data : "<unnamed>");
        return false;
    }

    uint8_t kind = ncc_type_spelling_is_root_const(type)
                       ? NCC_CT_STATIC_INIT_CONST_RO
                       : NCC_CT_STATIC_INIT_WRITABLE;
    uint8_t flags = ncc_type_spelling_is_pointer(type)
                        ? NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT
                        : 0;
    if (kind == NCC_CT_STATIC_INIT_CONST_RO
        && (flags & NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT) == 0) {
        fprintf(stderr,
                "ncc: const value-root static initializer '%s' is not "
                "supported by migrated baking yet\n",
                entry->name.data ? entry->name.data : "<unnamed>");
        ncc_free(type);
        return false;
    }

    ncc_ct_static_init_t *out = &(*static_inits)[*n_static_inits];
    out->name = ncc_string_from_raw(entry->name.data,
                                    (int64_t)entry->name.u8_bytes);
    out->typehash = ncc_type_hash_u64(type);
    out->kind = kind;
    out->flags = flags;
    out->degrade_ok = entry->static_init_needs_host_exec ? 1 : 0;
    (*n_static_inits)++;

    ncc_free(type);
    return true;
}

static int
compare_static_init_entries_by_source(const void *a, const void *b);

static bool
ncc_collect_static_inits(ncc_symtab_t *symtab,
                         ncc_ct_static_init_t **out_static_inits,
                         int *out_n_static_inits)
{
    *out_static_inits = nullptr;
    *out_n_static_inits = 0;
    if (!symtab) {
        return true;
    }

    ncc_ct_static_init_t *static_inits = nullptr;
    int n_static_inits = 0;
    int cap = 0;
    ncc_sym_entry_t **entries = nullptr;
    int n_entries = 0;
    int entries_cap = 0;

    for (ncc_scope_t *scope = symtab->all_scopes; scope;
         scope = scope->all_next) {
        for (ncc_sym_entry_t *entry = scope->first_in_scope; entry;
             entry = entry->next_in_scope) {
            if (entry->kind != NCC_SYM_VARIABLE || !entry->is_static_init
                || entry->is_comptime || entry->scope_depth != 1) {
                continue;
            }
            if (n_entries >= entries_cap) {
                int new_cap = entries_cap ? entries_cap * 2 : 4;
                entries = ncc_realloc(entries,
                                      (size_t)new_cap * sizeof(*entries));
                entries_cap = new_cap;
            }
            entries[n_entries++] = entry;
        }
    }

    qsort(entries, (size_t)n_entries, sizeof(*entries),
          compare_static_init_entries_by_source);

    for (int i = 0; i < n_entries; i++) {
        if (!ncc_push_static_init(&static_inits, &n_static_inits, &cap,
                                  entries[i])) {
            ncc_free(entries);
            ncc_free_static_inits(static_inits, n_static_inits);
            return false;
        }
    }

    ncc_free(entries);
    *out_static_inits = static_inits;
    *out_n_static_inits = n_static_inits;
    return true;
}

static int
compare_static_init_entries_by_source(const void *a, const void *b)
{
    const ncc_sym_entry_t *ea = *(const ncc_sym_entry_t * const *)a;
    const ncc_sym_entry_t *eb = *(const ncc_sym_entry_t * const *)b;
    uint32_t a_line = 0;
    uint32_t a_col = 0;
    uint32_t b_line = 0;
    uint32_t b_col = 0;

    ncc_xform_first_leaf_pos(ea ? ea->decl_node : nullptr, &a_line, &a_col);
    ncc_xform_first_leaf_pos(eb ? eb->decl_node : nullptr, &b_line, &b_col);

    if (a_line != b_line) {
        return a_line < b_line ? -1 : 1;
    }
    if (a_col != b_col) {
        return a_col < b_col ? -1 : 1;
    }
    return 0;
}

typedef enum {
    NCC_CRT_TARGET_LINUX,
    NCC_CRT_TARGET_DARWIN,
    NCC_CRT_TARGET_WINDOWS,
} ncc_crt_target_os_t;

static ncc_crt_target_os_t
ncc_crt_default_target_os(void)
{
#ifdef _WIN32
    return NCC_CRT_TARGET_WINDOWS;
#elif defined(__APPLE__)
    return NCC_CRT_TARGET_DARWIN;
#else
    return NCC_CRT_TARGET_LINUX;
#endif
}

static ncc_crt_target_os_t
ncc_crt_target_os_from_triple(const char *triple, ncc_crt_target_os_t fallback)
{
    if (!triple) {
        return fallback;
    }

    if (strstr(triple, "windows") || strstr(triple, "mingw")
        || strstr(triple, "msvc") || strstr(triple, "cygwin")) {
        return NCC_CRT_TARGET_WINDOWS;
    }
    if (strstr(triple, "darwin") || strstr(triple, "apple")
        || strstr(triple, "macos") || strstr(triple, "ios")
        || strstr(triple, "tvos") || strstr(triple, "watchos")) {
        return NCC_CRT_TARGET_DARWIN;
    }
    if (strstr(triple, "linux") || strstr(triple, "android")) {
        return NCC_CRT_TARGET_LINUX;
    }

    return fallback;
}

static ncc_crt_target_os_t
ncc_crt_target_os(const ncc_opts_t *opts)
{
    ncc_crt_target_os_t os = ncc_crt_default_target_os();

    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (strncmp(arg, "--target=", 9) == 0) {
            os = ncc_crt_target_os_from_triple(arg + 9, os);
            continue;
        }
        if (strncmp(arg, "-target=", 8) == 0) {
            os = ncc_crt_target_os_from_triple(arg + 8, os);
            continue;
        }
        if ((strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0)
            && i + 1 < opts->n_clang_args) {
            os = ncc_crt_target_os_from_triple(opts->clang_args[++i], os);
        }
    }

    return os;
}

static const char *
ncc_crt_platform_entry_flag(const ncc_opts_t *opts)
{
    switch (ncc_crt_target_os(opts)) {
    case NCC_CRT_TARGET_WINDOWS:
        return "-Wl,/entry:n00b_start";
    case NCC_CRT_TARGET_DARWIN:
        return "-Wl,-e,_n00b_start";
    case NCC_CRT_TARGET_LINUX:
    default:
        return "-Wl,-e,n00b_start";
    }
}

static const char *
ncc_crt_platform_entry_force_undefined_flag(const ncc_opts_t *opts)
{
    switch (ncc_crt_target_os(opts)) {
    case NCC_CRT_TARGET_DARWIN:
        return "-Wl,-u,_n00b_start";
    case NCC_CRT_TARGET_LINUX:
        return "-Wl,-u,n00b_start";
    case NCC_CRT_TARGET_WINDOWS:
    default:
        return nullptr;
    }
}

static bool
ncc_crt_compile_arg_takes_value(const char *arg)
{
    return strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0
           || strcmp(arg, "-arch") == 0 || strcmp(arg, "-isysroot") == 0
           || strcmp(arg, "--sysroot") == 0 || strcmp(arg, "-march") == 0
           || strcmp(arg, "-mcpu") == 0 || strcmp(arg, "-mtune") == 0
           || strcmp(arg, "-mabi") == 0;
}

static bool
ncc_crt_compile_arg_is_single(const char *arg)
{
    return strncmp(arg, "--target=", 9) == 0
           || strncmp(arg, "-target=", 8) == 0
           || strncmp(arg, "--sysroot=", 10) == 0
           || strncmp(arg, "-isysroot=", 10) == 0
           || strncmp(arg, "-mmacosx-version-min=", 21) == 0
           || strncmp(arg, "-miphoneos-version-min=", 24) == 0
           || strncmp(arg, "-mios-simulator-version-min=", 28) == 0
           || strncmp(arg, "-mtvos-version-min=", 19) == 0
           || strncmp(arg, "-mwatchos-version-min=", 22) == 0
           || strncmp(arg, "-march=", 7) == 0
           || strncmp(arg, "-mcpu=", 6) == 0
           || strncmp(arg, "-mtune=", 7) == 0
           || strncmp(arg, "-mfpu=", 6) == 0
           || strncmp(arg, "-mfloat-abi=", 12) == 0
           || strncmp(arg, "-mabi=", 6) == 0
           || strcmp(arg, "-m32") == 0 || strcmp(arg, "-m64") == 0
           || strcmp(arg, "-mthumb") == 0 || strcmp(arg, "-fPIC") == 0
           || strcmp(arg, "-fpic") == 0 || strcmp(arg, "-fPIE") == 0
           || strcmp(arg, "-fpie") == 0 || strcmp(arg, "-fno-pic") == 0
           || strcmp(arg, "-fno-pie") == 0;
}

static int
ncc_crt_append_compile_target_args(const ncc_opts_t *opts, const char **argv,
                                   int ai)
{
    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (ncc_crt_compile_arg_takes_value(arg)) {
            argv[ai++] = arg;
            if (i + 1 < opts->n_clang_args) {
                argv[ai++] = opts->clang_args[++i];
            }
            continue;
        }

        if (ncc_crt_compile_arg_is_single(arg)) {
            argv[ai++] = arg;
        }
    }

    return ai;
}

static bool
compile_crt_entry_object(const ncc_opts_t *opts, ncc_temp_workspace_t *workspace,
                         char **object_path_out)
{
    *object_path_out = nullptr;

    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(workspace, "ncc_crt_", &tmp_err)) {
        print_process_message("failed to create crt entry temp path", tmp_err);
        ncc_free(tmp_err);
        return false;
    }

    const char *entry_source =
        ncc_crt_emit_entry(opts, NCC_CRT_VARIANT_BASE);
    if (!entry_source) {
        fprintf(stderr, "ncc: failed to generate custom crt entry source\n");
        return false;
    }

    char *source_path = ncc_temp_workspace_join(workspace, "entry.c");
    char *object_path = ncc_temp_workspace_join(workspace, "entry.o");

    if (!source_path || !object_path) {
        fprintf(stderr, "ncc: failed to create custom crt entry temp files\n");
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    char *write_err = nullptr;
    if (!ncc_platform_write_file(source_path, entry_source,
                                 strlen(entry_source), &write_err)) {
        print_process_message("write to custom crt entry temp file failed",
                              write_err);
        ncc_free(write_err);
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    int          max_args = 8 + opts->n_clang_args;
    const char **argv     = ncc_alloc_array(const char *, (size_t)max_args);
    int          ai       = 0;

    argv[ai++] = opts->compiler;
    ai         = ncc_crt_append_compile_target_args(opts, argv, ai);
    argv[ai++] = "-std=gnu23";
    argv[ai++] = "-c";
    argv[ai++] = source_path;
    argv[ai++] = "-o";
    argv[ai++] = object_path;
    argv[ai]   = nullptr;

    ncc_verbose("custom crt entry source=%s", source_path);
    ncc_verbose("custom crt entry object=%s", object_path);
    ncc_verbose_args("custom crt entry compiler argv", argv, ai);

    ncc_process_spec_t spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = verbose,
        .capture_stderr = true,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch custom crt entry compiler",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    forward_process_output("custom crt entry compiler stdout",
                           proc.stdout_data, proc.stdout_len, stdout);
    forward_process_output("custom crt entry compiler stderr",
                           proc.stderr_data, proc.stderr_len, stderr);

    int rc = proc.exit_code;
    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_free(source_path);

    if (rc != 0) {
        fprintf(stderr, "ncc: custom crt entry compilation failed (exit %d)\n",
                rc);
        ncc_free(object_path);
        return false;
    }

    *object_path_out = object_path;
    return true;
}

static int
custom_entry_link_passthrough(const ncc_opts_t *opts, int argc,
                              const char **orig_argv)
{
    ncc_verbose("custom-entry passthrough link to %s", opts->compiler);

    ncc_opts_t link_opts = *opts;
    if (!link_opts.input_file) {
        link_opts.input_file = "<link>";
    }

    ncc_temp_workspace_t crt_workspace = {0};
    char *crt_entry_object = nullptr;

    if (!compile_crt_entry_object(&link_opts, &crt_workspace,
                                  &crt_entry_object)) {
        ncc_temp_workspace_cleanup(&crt_workspace);
        return 1;
    }

    const char **argv = ncc_alloc_array(const char *, (size_t)(argc + 9));
    int          ai   = 0;

    argv[ai++] = opts->compiler;
    argv[ai++] = "-nostartfiles";
    argv[ai++] = ncc_crt_platform_entry_flag(opts);
    const char *entry_force =
        ncc_crt_platform_entry_force_undefined_flag(opts);
    if (entry_force) {
        argv[ai++] = entry_force;
    }
    argv[ai++] = "-x";
    argv[ai++] = "none";
    argv[ai++] = crt_entry_object;

    for (int i = 1; i < argc; i++) {
        const char *arg = orig_argv[i];

        if (strncmp(arg, "--ncc-", 6) == 0 || strcmp(arg, "--no-ncc") == 0) {
            if (ncc_arg_is_ncc_only_with_value(arg) && i + 1 < argc) {
                i++;
            }
            continue;
        }

        argv[ai++] = arg;
    }

    argv[ai] = nullptr;

    ncc_verbose_args("custom-entry passthrough linker argv", argv, ai);

    ncc_process_spec_t spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = verbose,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch custom-entry linker",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(crt_entry_object);
        ncc_temp_workspace_cleanup(&crt_workspace);
        return 1;
    }

    forward_process_output("custom-entry linker stdout",
                           proc.stdout_data, proc.stdout_len, stdout);
    forward_process_output("custom-entry linker stderr",
                           proc.stderr_data, proc.stderr_len, stderr);

    int rc = proc.exit_code;
    ncc_verbose("custom-entry linker exit code=%d", rc);
    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_free(crt_entry_object);
    ncc_temp_workspace_cleanup(&crt_workspace);
    return rc;
}

static int
pipe_to_compiler(const ncc_opts_t *opts, const char *c_source, size_t c_len)
{
    // Build argv: compiler [-std=gnu23] [flags...] -x c temp.c
    //             [-x none linker-inputs...]
    // clang_args already includes -c, -o, and all passthrough flags.
    // Linker inputs (.a, .o, .so) must come after the transformed source with a
    // "-x none" reset so clang treats them correctly on all platforms.
    bool use_custom_entry = opts->custom_entry && ncc_is_link_invocation(opts);
    ncc_temp_workspace_t crt_workspace = {0};
    char *crt_entry_object = nullptr;
    ncc_temp_workspace_t source_workspace = {0};
    char *source_path = nullptr;

    if (use_custom_entry
        && !compile_crt_entry_object(opts, &crt_workspace, &crt_entry_object)) {
        ncc_temp_workspace_cleanup(&crt_workspace);
        return 1;
    }

    source_path = write_transformed_source_temp(&source_workspace,
                                                "ncc_compile_",
                                                c_source,
                                                c_len);
    if (!source_path) {
        ncc_free(crt_entry_object);
        ncc_temp_workspace_cleanup(&crt_workspace);
        return 1;
    }

    int          max_args = 16 + opts->n_clang_args * 2
                            + (use_custom_entry ? 9 : 0);
    const char **argv     = ncc_alloc_array(const char *, (size_t)(max_args + 1));
    int          ai       = 0;

    argv[ai++] = opts->compiler;

    if (!opts->has_std) {
        argv[ai++] = "-std=gnu23";
    }

    // Suppress ODR warnings from alignas attributes in transformed code.
    argv[ai++] = "-Wno-odr";

    if (use_custom_entry) {
        argv[ai++] = "-nostartfiles";
        argv[ai++] = ncc_crt_platform_entry_flag(opts);
        const char *entry_force =
            ncc_crt_platform_entry_force_undefined_flag(opts);
        if (entry_force) {
            argv[ai++] = entry_force;
        }
    }

    // Pass compiler flags (non-linker-input args) before -x c -.
    // Track -o so its argument (often ending in .o) isn't mistaken
    // for a linker input and moved after "-x c -".
    for (int i = 0; i < opts->n_clang_args; i++) {
        if (strcmp(opts->clang_args[i], "-o") == 0) {
            argv[ai++] = opts->clang_args[i];
            if (i + 1 < opts->n_clang_args) {
                argv[ai++] = opts->clang_args[++i];
            }
        }
        else if (strcmp(opts->clang_args[i], "-include") == 0) {
            i++;
        }
        else if (!is_linker_input(opts->clang_args[i])) {
            argv[ai++] = opts->clang_args[i];
        }
    }

    argv[ai++] = "-x";
    argv[ai++] = "c";
    argv[ai++] = source_path;

    // Append linker inputs (.a, .o, etc.) after the transformed source, resetting
    // the language so clang doesn't misinterpret them as C source.
    bool need_reset = true;
    if (use_custom_entry) {
        argv[ai++] = "-x";
        argv[ai++] = "none";
        need_reset = false;
        argv[ai++] = crt_entry_object;
    }
    for (int i = 0; i < opts->n_clang_args; i++) {
        if (strcmp(opts->clang_args[i], "-o") == 0) {
            i++; // skip -o and its argument
            continue;
        }
        if (is_linker_input(opts->clang_args[i])) {
            if (need_reset) {
                argv[ai++] = "-x";
                argv[ai++] = "none";
                need_reset  = false;
            }
            argv[ai++] = opts->clang_args[i];
        }
    }

    argv[ai] = nullptr;

    ncc_verbose("compiler source bytes=%zu path=%s", c_len, source_path);
    ncc_verbose_args("compiler argv", argv, ai);

    ncc_process_spec_t   spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = verbose,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch compiler", proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(source_path);
        ncc_free(crt_entry_object);
        ncc_temp_workspace_cleanup(&source_workspace);
        ncc_temp_workspace_cleanup(&crt_workspace);
        return 1;
    }

    forward_process_output("compiler stdout", proc.stdout_data,
                           proc.stdout_len, stdout);
    forward_process_output("compiler stderr", proc.stderr_data,
                           proc.stderr_len, stderr);

    ncc_free(argv);
    ncc_free(source_path);
    ncc_free(crt_entry_object);
    ncc_temp_workspace_cleanup(&source_workspace);
    ncc_temp_workspace_cleanup(&crt_workspace);
    int rc = proc.exit_code;
    ncc_verbose("compiler exit code=%d", rc);
    ncc_process_result_free(&proc);
    return rc;
}

// ============================================================================
// Dependency file generation
// ============================================================================
//
// When -MD or -MMD is present, run a separate clang -M process on the
// original source file to capture real #include dependencies.  This must
// use the original source (not our transformed stdin) so the dependency
// file lists actual header paths.

static char *
derive_depfile_path(const ncc_opts_t *opts)
{
    const char *path = opts->output_file ? opts->output_file : opts->input_file;
    const char *last_sep = nullptr;
    const char *last_dot = nullptr;

    for (const char *p = path; p && *p; p++) {
        if (path_is_sep(*p)) {
            last_sep = p;
            last_dot = nullptr;
        }
        else if (*p == '.') {
            last_dot = p;
        }
    }

    const char *base = last_sep ? last_sep + 1 : path;
    size_t prefix_len = last_dot && last_dot >= base
                            ? (size_t)(last_dot - path)
                            : strlen(path);
    char *result = ncc_alloc_size(1, prefix_len + 3);
    memcpy(result, path, prefix_len);
    memcpy(result + prefix_len, ".d", 3);
    return result;
}

static bool
depfile_is_nonempty(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    int ch = fgetc(f);
    fclose(f);
    return ch != EOF;
}

static int
generate_depfile(const ncc_opts_t *opts)
{
    if (!opts->has_dep_flags) {
        return 0;
    }

    char *derived_depfile = nullptr;
    const char *depfile_path = opts->dep_mf;
    if (!depfile_path) {
        derived_depfile = derive_depfile_path(opts);
        depfile_path = derived_depfile;
    }

    // Build argv: compiler -M [-MM] -MF file [-MQ target] [-MT target]
    //             [-std=...] [clang_args...] source.c
    int          max = 16 + opts->n_clang_args;
    const char **argv = ncc_alloc_array(const char *, (size_t)(max + 1));
    int          ai   = 0;

    argv[ai++] = opts->compiler;

    if (opts->dep_mmmd) {
        argv[ai++] = "-MM";
    }
    else {
        argv[ai++] = "-M";
    }

    argv[ai++] = "-fno-blocks";

    if (!opts->has_std) {
        argv[ai++] = "-std=gnu23";
    }

    argv[ai++] = "-MF";
    argv[ai++] = depfile_path;

    if (opts->dep_mq) {
        argv[ai++] = "-MQ";
        argv[ai++] = opts->dep_mq;
    }
    if (opts->dep_mt) {
        argv[ai++] = "-MT";
        argv[ai++] = opts->dep_mt;
    }
    else if (!opts->dep_mq && opts->output_file) {
        argv[ai++] = "-MT";
        argv[ai++] = opts->output_file;
    }

    // Pass through include paths, defines, and other relevant flags.
    for (int i = 0; i < opts->n_clang_args; i++) {
        const char *a = opts->clang_args[i];
        // Skip -c and -o (irrelevant for dep generation).
        if (strcmp(a, "-c") == 0) {
            continue;
        }
        if (strcmp(a, "-o") == 0) {
            i++;
            continue;
        }
        if (strncmp(a, "-o", 2) == 0 && a[2] != '\0') {
            continue;
        }
        argv[ai++] = a;
    }

    argv[ai++] = opts->input_file;
    argv[ai]   = nullptr;

    ncc_verbose_args("depfile argv", argv, ai);

    ncc_process_spec_t   spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = verbose,
        .capture_stderr = verbose,
    };
    ncc_process_result_t proc;

    if (!ncc_process_run(&spec, &proc)) {
        print_process_message("failed to launch depfile compiler",
                              proc.stderr_data);
        ncc_process_result_free(&proc);
        ncc_free(argv);
        ncc_free(derived_depfile);
        return 1;
    }

    forward_process_output("depfile stdout", proc.stdout_data,
                           proc.stdout_len, stdout);
    forward_process_output("depfile stderr", proc.stderr_data,
                           proc.stderr_len, stderr);

    ncc_free(argv);
    int rc = proc.exit_code;
    ncc_verbose("depfile exit code=%d", rc);
    ncc_process_result_free(&proc);
    if (rc == 0 && !depfile_is_nonempty(depfile_path)) {
        fprintf(stderr, "ncc: depfile was not created or is empty: %s\n",
                depfile_path);
        rc = 1;
    }
    ncc_free(derived_depfile);
    return rc;
}

// ============================================================================
// Main pipeline: preprocess → tokenize → parse → transform → emit
// ============================================================================

static int
compile_file(ncc_opts_t *opts)
{
    ncc_verbose("compile start: input=%s output=%s compiler=%s mode=%s",
                opts->input_file,
                opts->output_file ? opts->output_file : "(default)",
                opts->compiler,
                opts->has_E ? "emit-c"
                            : opts->has_c ? "compile-only"
                                          : "compile-and-link");

    // Stage 1: Load grammar.
    ncc_grammar_t *g = load_c_grammar(opts);
    if (!g) {
        return 1;
    }

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after grammar");
#endif

    // Stage 2: Preprocess with clang -E.
    size_t pp_len  = 0;
    char  *pp_text = run_preprocessor(opts, &pp_len);
    if (!pp_text) {
        ncc_grammar_free(g);
        return 1;
    }

    ncc_verbose("preprocessed %zu bytes", pp_len);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after preprocess");
#endif

    // Stage 3: Tokenize.
    ncc_verbose("tokenizing preprocessed source");
    ncc_buffer_t              *buf      = ncc_buffer_from_bytes(pp_text,
                                                                  (int64_t)pp_len);
    ncc_c_tokenizer_state_t   *tok_state = ncc_c_tokenizer_state_new();
    ncc_scanner_t             *scanner   = ncc_scanner_new(
        buf, ncc_c_tokenize, g,
        ncc_option_set(ncc_string_t,
                        ncc_string_from_cstr(opts->input_file)),
        tok_state, ncc_c_tokenizer_reset);
    ncc_token_stream_t *ts = ncc_token_stream_new(scanner);

    // Store tokenizer callback in grammar so ncc_xform_parse_template can use it.
    g->tokenize_cb = (void *)ncc_c_tokenize;

    ncc_free(pp_text);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after tokenize");
#endif

    // Stage 3.5: Token dump (before parsing).
    if (opts->dump_tokens) {
        // Force full tokenization by scanning all tokens.
        while (ncc_stream_next(ts)) {
        }
        dump_tokens(ts, stderr);
    }

    // Stage 4: Parse with PWZ.
    ncc_pwz_parser_t *parser = ncc_pwz_new(g);
    assert(parser);

    ncc_verbose("parsing...");

    bool ok = ncc_pwz_parse(parser, ts);

    if (!ok) {
        int32_t ntokens = ts->token_count;
        fprintf(stderr, "ncc: parse FAILED (%d tokens produced)\n", ntokens);
        maybe_print_array_literal_parse_hint(ts);

        int32_t show = 10;
        if (ntokens > 0) {
            fprintf(stderr, "  first %d tokens:\n",
                    show < ntokens ? show : ntokens);
            for (int32_t i = 0; i < ntokens && i < show; i++) {
                ncc_token_info_t *t = ts->tokens[i];
                if (t) {
                    fprintf(stderr, "    [%d] tid=%d", i, t->tid);
                    if (ncc_option_is_set(t->value)) {
                        ncc_string_t v = ncc_option_get(t->value);
                        if (v.data) {
                            fprintf(stderr, " \"%.*s\"",
                                    v.u8_bytes > 40 ? 40 : (int)v.u8_bytes,
                                    v.data);
                        }
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (ntokens > show * 2) {
                fprintf(stderr, "  last %d tokens:\n", show);
                for (int32_t i = ntokens - show; i < ntokens; i++) {
                    ncc_token_info_t *t = ts->tokens[i];
                    if (t) {
                        fprintf(stderr, "    [%d] tid=%d", i, t->tid);
                        if (ncc_option_is_set(t->value)) {
                            ncc_string_t v = ncc_option_get(t->value);
                            if (v.data) {
                                fprintf(stderr, " \"%.*s\"",
                                        v.u8_bytes > 40
                                            ? 40
                                            : (int)v.u8_bytes,
                                        v.data);
                            }
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
        }

        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    ncc_parse_tree_t *tree = ncc_pwz_get_tree(parser);
    if (!tree) {
        fprintf(stderr, "ncc: parse succeeded but no tree produced\n");
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    ncc_verbose("parse OK (%d tokens)", ts->token_count);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after parse");
    ncc_pwz_report_stats();
#endif

    // Stage 5: Reclassify walk (typedef tracking).  Runs on the provisional
    // tree (extracted under the default last-alternative policy). typedef
    // DECLARATIONS are unambiguous, so this classifies every typedef name
    // correctly regardless of how their USE sites were provisionally parsed.
    int32_t reclassified = ncc_typedef_walk(
        g, tree, ts->tokens, ts->token_count);

    if (reclassified > 0) {
        ncc_verbose("reclassified %d tokens", reclassified);
    }

    // Stage 5b (TS-5): type-aware re-extraction. Build a provisional symbol
    // table from the provisional tree, install it as the forest->tree
    // disambiguation oracle, and re-extract the retained forest so the C
    // typedef/identifier ambiguity (e.g. `lp[2]` as a value subscript vs an
    // array-of-`lp` type) is resolved at the parse layer rather than patched up
    // by downstream xforms. The provisional symtab's typedef classification is
    // reliable because it derives from unambiguous declarations.
    //
    // Iterate to a fixed point (bounded): the first pass resolves ambiguities
    // visible from unambiguous declarations (typedefs), which can in turn make
    // a previously mis-parsed definition (e.g. `int f(int v){…}` where `f`
    // matched the function-specifier soft keyword) parse correctly — so the
    // next pass's symbol table knows `f` is a value and can fix its call sites
    // (`f(x);` mis-read as a declaration). Two passes suffice in practice.
    for (int pass = 0; pass < 2; pass++) {
        ncc_symtab_t *prov_symtab = ncc_populate_symbols(g, tree);

        ncc_pwz_set_disambiguator(parser, ncc_disambig_eval, prov_symtab);
        tree = ncc_pwz_reextract(parser);
        ncc_pwz_set_disambiguator(parser, nullptr, nullptr);

        ncc_symtab_free(prov_symtab);
    }

    // Release PWZ per-parse intermediate state (arena, worklists) now that the
    // final tree has been (re-)extracted.  Drops peak live memory before the
    // transform and emit stages run.
    ncc_pwz_release_parse_state(parser);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after arena release");
#endif

    // Stage 5.5: Dump pre-transform tree.
    if (opts->dump_tree) {
        ncc_tree_dump(g, tree, stderr, opts->dump_tree_raw);
    }

    // Stage 6: Transform passes (typeid, typestr, typehash).
    ncc_verbose("registering transforms");
    ncc_xform_registry_t xreg;
    ncc_xform_registry_init(&xreg, g);
    ncc_register_array_literal_xform(&xreg);
    // rpc must run first — its synthesized bodies reference the
    // type-mangled identifiers that typeid / option / generic_struct /
    // kargs_vargs produce; running after any of those would see the
    // already-mangled signatures.
    ncc_register_rpc_xform(&xreg);
    // _try lowers result-error propagation into a statement expression with an
    // early return. Runs before generic_struct/typeid/defer/gc so the emitted
    // result type, return, and any type-queries in the operand flow through.
    ncc_register_try_xform(&xreg);
    // Strip `?` nullable qualifiers before any type-inspecting pass; the
    // nullability info is read from the symbol table (built earlier) by the
    // null-state analysis.
    ncc_register_nullable_xform(&xreg);
    ncc_register_generic_struct_xform(&xreg);
    // _defer lowers a function body's defers into source at each scope exit.
    // Runs early so the relocated bodies still flow through the type-query,
    // option, kargs, and contracts passes below.
    ncc_register_defer_xform(&xreg);
    ncc_register_typeid_xform(&xreg);
    ncc_register_option_xform(&xreg);
    ncc_register_typestr_xform(&xreg);
    ncc_register_typehash_xform(&xreg);
    ncc_register_kargs_vargs_xform(&xreg);
    ncc_register_once_xform(&xreg);
    ncc_register_contracts_xform(&xreg);
    ncc_register_bang_xform(&xreg);
    ncc_register_rstr_xform(&xreg);
    ncc_register_static_image_xform(&xreg);
    ncc_register_gc_stack_maps_xform(&xreg);
    ncc_register_constexpr_xform(&xreg);
    ncc_register_constexpr_paste_xform(&xreg);
    ncc_register_static_init_xform(&xreg);
    // gc_globals must run LAST: it walks the final flattened TU
    // (including any synthetic decls earlier passes — rpc, once,
    // kargs_vargs, static_image — produced) per spec § 8.3.
    ncc_register_gc_globals_xform(&xreg);

    // Template registry for rstr (and future template-based transforms).
    ncc_template_registry_t tmpl_reg;
    ncc_template_registry_init(&tmpl_reg, g, ncc_c_tokenize);

    // Default rstr templates (simple ncc_string_t compound literal, no GC header).
    static const char *default_rstr_styled =
        "({$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,"
        ".codepoints=$4,.styling=$5};&$1;})";
    static const char *default_rstr_plain =
        "({static ncc_string_t $0={.u8_bytes=$1,.data=$2,"
        ".codepoints=$3,.styling=((void*)0)};&$0;})";
    static const char *default_rstr_static_ref_styled =
        "$0 static ncc_string_t $1={.u8_bytes=$2,.data=$3,"
        ".codepoints=$4,.styling=$5};";
    static const char *default_rstr_static_ref_plain =
        "static ncc_string_t $0={.u8_bytes=$1,.data=$2,"
        ".codepoints=$3,.styling=((void*)0)};";
    static const char *default_rstr_static_ref_expr_styled = "&$1";
    static const char *default_rstr_static_ref_expr_plain  = "&$0";
    static const char *default_array_literal_data_template =
        "static $0 $1[]={$3};";
    static const char *default_array_literal_data_expr = "$1";
    static const char *default_static_object_entry_attr = "";

    // Resolution order: CLI flag > meson define > built-in default.
    const char *rstr_styled = default_rstr_styled;
    const char *rstr_plain  = default_rstr_plain;
    const char *rstr_static_ref_styled =
        default_rstr_static_ref_styled;
    const char *rstr_static_ref_plain =
        default_rstr_static_ref_plain;
    const char *rstr_static_ref_expr_styled =
        default_rstr_static_ref_expr_styled;
    const char *rstr_static_ref_expr_plain =
        default_rstr_static_ref_expr_plain;
    const char *array_literal_data_template =
        default_array_literal_data_template;
    const char *array_literal_data_expr = default_array_literal_data_expr;
    const char *static_object_entry_attr =
        default_static_object_entry_attr;

#ifdef NCC_RSTR_TEMPLATE_STYLED
    rstr_styled = NCC_RSTR_TEMPLATE_STYLED;
#endif
#ifdef NCC_RSTR_TEMPLATE_PLAIN
    rstr_plain = NCC_RSTR_TEMPLATE_PLAIN;
#endif
#ifdef NCC_RSTR_STATIC_REF_TEMPLATE_STYLED
    rstr_static_ref_styled = NCC_RSTR_STATIC_REF_TEMPLATE_STYLED;
#endif
#ifdef NCC_RSTR_STATIC_REF_TEMPLATE_PLAIN
    rstr_static_ref_plain = NCC_RSTR_STATIC_REF_TEMPLATE_PLAIN;
#endif
#ifdef NCC_RSTR_STATIC_REF_EXPR_STYLED
    rstr_static_ref_expr_styled = NCC_RSTR_STATIC_REF_EXPR_STYLED;
#endif
#ifdef NCC_RSTR_STATIC_REF_EXPR_PLAIN
    rstr_static_ref_expr_plain = NCC_RSTR_STATIC_REF_EXPR_PLAIN;
#endif
#ifdef NCC_ARRAY_LITERAL_DATA_TEMPLATE
    array_literal_data_template = NCC_ARRAY_LITERAL_DATA_TEMPLATE;
#endif
#ifdef NCC_ARRAY_LITERAL_DATA_EXPR
    array_literal_data_expr = NCC_ARRAY_LITERAL_DATA_EXPR;
#endif
#ifdef NCC_STATIC_OBJECT_ENTRY_ATTR
    static_object_entry_attr = NCC_STATIC_OBJECT_ENTRY_ATTR;
#endif

    if (opts->rstr_template_styled) {
        rstr_styled = opts->rstr_template_styled;
    }
    if (opts->rstr_template_plain) {
        rstr_plain = opts->rstr_template_plain;
    }
    if (opts->rstr_static_ref_template_styled) {
        rstr_static_ref_styled = opts->rstr_static_ref_template_styled;
    }
    if (opts->rstr_static_ref_template_plain) {
        rstr_static_ref_plain = opts->rstr_static_ref_template_plain;
    }
    if (opts->rstr_static_ref_expr_styled) {
        rstr_static_ref_expr_styled = opts->rstr_static_ref_expr_styled;
    }
    if (opts->rstr_static_ref_expr_plain) {
        rstr_static_ref_expr_plain = opts->rstr_static_ref_expr_plain;
    }
    if (opts->array_literal_data_template) {
        array_literal_data_template = opts->array_literal_data_template;
    }
    if (opts->array_literal_data_expr) {
        array_literal_data_expr = opts->array_literal_data_expr;
    }
    if (opts->static_object_entry_attr) {
        static_object_entry_attr = opts->static_object_entry_attr;
    }

    ncc_template_register(&tmpl_reg, "rstr_styled",
                           "primary_expression", rstr_styled);
    ncc_template_register(&tmpl_reg, "rstr_plain",
                           "primary_expression", rstr_plain);

    // Bang (!) error propagation template.
    static const char bang_tmpl[] = {
#embed "templates/bang.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "bang", "primary_expression", bang_tmpl);

    // @rpc(...) templates — one per stream shape. Parsed as
    // translation_unit because each expands to multiple external
    // declarations (dispatcher, constructor, client stub).
    static const char rpc_unary_tmpl[] = {
#embed "templates/rpc_unary.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "rpc_unary", "translation_unit",
                          rpc_unary_tmpl);

    static const char rpc_server_stream_tmpl[] = {
#embed "templates/rpc_server_stream.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "rpc_server_stream",
                          "translation_unit", rpc_server_stream_tmpl);

    static const char rpc_client_stream_tmpl[] = {
#embed "templates/rpc_client_stream.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "rpc_client_stream",
                          "translation_unit", rpc_client_stream_tmpl);

    static const char rpc_bidi_tmpl[] = {
#embed "templates/rpc_bidi.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "rpc_bidi", "translation_unit",
                          rpc_bidi_tmpl);

    // Resolve vargs_type, once_prefix, rstr types:
    // CLI > meson define > default.
    const char *vargs_type             = "ncc_vargs_t";
    const char *once_prefix            = "__ncc_";
    const char *rstr_string_type       = "ncc_string_t*";
    const char *rstr_text_style_type   = "ncc_text_style_t";
    const char *rstr_style_record_type = "ncc_style_record_t";

#ifdef NCC_VARGS_TYPE
    vargs_type = NCC_VARGS_TYPE;
#endif
#ifdef NCC_ONCE_PREFIX
    once_prefix = NCC_ONCE_PREFIX;
#endif
#ifdef NCC_RSTR_STRING_TYPE
    rstr_string_type = NCC_RSTR_STRING_TYPE;
#endif
#ifdef NCC_RSTR_TEXT_STYLE_TYPE
    rstr_text_style_type = NCC_RSTR_TEXT_STYLE_TYPE;
#endif
#ifdef NCC_RSTR_STYLE_RECORD_TYPE
    rstr_style_record_type = NCC_RSTR_STYLE_RECORD_TYPE;
#endif

    if (opts->vargs_type) {
        vargs_type = opts->vargs_type;
    }
    if (opts->once_prefix) {
        once_prefix = opts->once_prefix;
    }
    if (opts->rstr_string_type) {
        rstr_string_type = opts->rstr_string_type;
    }
    if (opts->rstr_text_style_type) {
        rstr_text_style_type = opts->rstr_text_style_type;
    }
    if (opts->rstr_style_record_type) {
        rstr_style_record_type = opts->rstr_style_record_type;
    }

    ncc_xform_ctx_t xctx;
    ncc_xform_ctx_init(&xctx, g, &xreg, tree);
    ncc_xform_data_t xdata = {
        .compiler          = opts->compiler,
        .input_file        = opts->input_file,
        .constexpr_headers = opts->constexpr_headers,
        .template_reg      = &tmpl_reg,
        .vargs_type        = vargs_type,
        .once_prefix       = once_prefix,
        .rstr_string_type  = rstr_string_type,
        .rstr_text_style_type   = rstr_text_style_type,
        .rstr_style_record_type = rstr_style_record_type,
        .rstr_static_ref_template_styled = rstr_static_ref_styled,
        .rstr_static_ref_template_plain  = rstr_static_ref_plain,
        .rstr_static_ref_expr_styled = rstr_static_ref_expr_styled,
        .rstr_static_ref_expr_plain  = rstr_static_ref_expr_plain,
        .array_literal_data_template = array_literal_data_template,
        .array_literal_data_expr     = array_literal_data_expr,
        .static_object_entry_attr    = static_object_entry_attr,
        .static_identity_generate_namespace =
            opts->static_identity_generate_namespace,
        .gc_stack_maps               = opts->gc_stack_maps,
        .gc_stack_maps_relaxed       = opts->gc_stack_maps_relaxed,
        .gc_typemaps                 = opts->gc_typemaps,
        .auto_gc_roots               = opts->auto_gc_roots,
        .gcmap_prelink               = opts->gcmap_prelink,
    };
    ncc_dict_init(&xdata.func_meta,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.option_meta,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.option_decls,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.generic_struct_decls,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.array_types,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.list_types,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.dict_types,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.gc_aggregate_types,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.gc_pointer_typedefs,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.gc_function_pointer_typedefs,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    // Parent pointers let expression typing map an expression back to its
    // lexical scope (scope_for_expr walks up to the enclosing scope node).
    // Set before any ncc_type_of_expr use (nodiscard below, typehash/typeid/
    // typestr xforms later); xform mutations keep them current via set_child.
    ncc_xform_set_parent_pointers(tree);

    // Build the scoped symbol table for the type model. Runs after the typedef
    // reclassification walk so declarator names are settled.
    xdata.symtab = ncc_populate_symbols(g, tree);

    // Flow-sensitive null-state analysis: must run before transforms strip the
    // `?` nullable markers from the tree.
    ncc_nullability_check(g, tree, xdata.symtab);

    // Must-check results: warn on a discarded result-protocol return value.
    // Runs on the fully-structured pre-transform tree (like nullability);
    // `_try`, which consumes a result, is still spelled `_try E` here and is
    // skipped by the discard check.
    ncc_nodiscard_check(g, tree, xdata.symtab);

    // Traditional-union deprecation. Runs PRE-transform (here, alongside the
    // other read-only lint passes) so n00b_variant_t value-unions are still in
    // their `_generic_struct typeid("n00b_variant", ...)` source form and are
    // excluded by that marker — after _generic_struct lowering they become
    // synthesized, file-less, parent-less nodes that no type query can resolve.
    // `union_error` gates the final exit code when --ncc-error-on-union is set
    // and a bare union was found (the diagnostics are emitted at error severity
    // above), avoiding a mid-pipeline teardown.
    int n_union_diags = ncc_union_deprecation_check(tree,
                                                    opts->allow_unions,
                                                    opts->error_on_union);
    bool union_error = opts->error_on_union && n_union_diags > 0;

    bool has_optional_comptime = false;
    if (ncc_comptime_check(g, tree, xdata.symtab, &has_optional_comptime) > 0) {
        ncc_symtab_free(xdata.symtab);
        xdata.symtab = nullptr;
        ncc_dict_free(&xdata.func_meta);
        ncc_dict_free(&xdata.option_meta);
        ncc_dict_free(&xdata.option_decls);
        ncc_dict_free(&xdata.generic_struct_decls);
        ncc_dict_free(&xdata.array_types);
        ncc_dict_free(&xdata.list_types);
        ncc_dict_free(&xdata.dict_types);
        ncc_dict_free(&xdata.gc_aggregate_types);
        ncc_dict_free(&xdata.gc_pointer_typedefs);
        ncc_dict_free(&xdata.gc_function_pointer_typedefs);
        free_gc_stack_roots(xdata.gc_stack_roots);
        ncc_template_registry_free(&tmpl_reg);
        ncc_xform_registry_free(&xreg);
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    ncc_ct_static_init_t *static_inits = nullptr;
    int                   n_static_inits = 0;
    bool static_inits_ok = ncc_collect_static_inits(xdata.symtab,
                                                    &static_inits,
                                                    &n_static_inits);
    if (!static_inits_ok) {
        ncc_symtab_free(xdata.symtab);
        xdata.symtab = nullptr;
        ncc_dict_free(&xdata.func_meta);
        ncc_dict_free(&xdata.option_meta);
        ncc_dict_free(&xdata.option_decls);
        ncc_dict_free(&xdata.generic_struct_decls);
        ncc_dict_free(&xdata.array_types);
        ncc_dict_free(&xdata.list_types);
        ncc_dict_free(&xdata.dict_types);
        ncc_dict_free(&xdata.gc_aggregate_types);
        ncc_dict_free(&xdata.gc_pointer_typedefs);
        ncc_dict_free(&xdata.gc_function_pointer_typedefs);
        free_gc_stack_roots(xdata.gc_stack_roots);
        ncc_template_registry_free(&tmpl_reg);
        ncc_xform_registry_free(&xreg);
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    xctx.user_data = &xdata;
    ncc_verbose("applying transforms");
    tree = ncc_xform_apply(&xreg, &xctx);

    if (xctx.nodes_replaced > 0) {
        ncc_verbose("transforms: %d nodes replaced", xctx.nodes_replaced);
    }

    // Rebuild the symbol table against the fully-transformed tree so emit-time
    // type resolution sees resolved forms (e.g. _generic_struct lowered to
    // `struct __mangled`). The pre-transform table served expression typing
    // during the passes; gc-typemap emission needs the post-transform view.
    if (xdata.symtab) {
        ncc_symtab_free(xdata.symtab);
    }
    xdata.symtab = ncc_populate_symbols(g, tree);

    // WP-020 / D-049: emit link-time GC pointer-map descriptors for the
    // pointer-to-aggregate types this TU typehash'd. Must run before the
    // transform dicts (gc_aggregate_types) are freed below; appended to the
    // pretty-printed output further down.
    char *gc_typemap_text = ncc_gc_typemap_emit(&xctx);

    bool         has_comptime_main = ncc_has_function_definition_named(
        tree, "comptime_main");
    ncc_ct_sig_t comptime_sig = {
        .argc     = 3,
        .has_argv = true,
        .has_envp = true,
    };
    uint8_t comptime_main_flags =
        has_optional_comptime ? NCC_CT_MAIN_FLAG_OPTIONAL : 0;
    ncc_ct_var_t *comptime_vars = nullptr;
    int           n_comptime_vars = 0;
    bool comptime_vars_ok = ncc_collect_comptime_vars(xdata.symtab,
                                                      &comptime_vars,
                                                      &n_comptime_vars);

    // (The traditional-union deprecation check runs pre-transform, above, where
    // the [[n00b::raw_union]] escape hatch is still present — before the strip
    // below removes every ncc-only attribute.)

    // Strip EVERY remaining `[[n00b::*]]` attribute now — AFTER every pass that
    // consumes one has read it (noscan by the GC typemap above; comptime by
    // comptime-check; raw_union by the union-deprecation check above; nogc is
    // already stripped inside the GC stack-map xform) and BEFORE the tree is
    // pretty-printed — so clang never sees an ncc-dialect attribute (and never
    // warns "unknown attribute"). A blanket strip means new ncc-only attributes
    // are handled automatically.
    ncc_xform_strip_n00b_named_attribute_specifiers(tree, nullptr);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after transform");
#endif

    ncc_dict_free(&xdata.func_meta);
    ncc_dict_free(&xdata.option_meta);
    ncc_dict_free(&xdata.option_decls);
    ncc_dict_free(&xdata.generic_struct_decls);
    ncc_dict_free(&xdata.array_types);
    ncc_dict_free(&xdata.list_types);
    ncc_dict_free(&xdata.dict_types);
    ncc_dict_free(&xdata.gc_aggregate_types);
    ncc_dict_free(&xdata.gc_pointer_typedefs);
    ncc_dict_free(&xdata.gc_function_pointer_typedefs);
    if (xdata.symtab) {
        ncc_symtab_free(xdata.symtab);
        xdata.symtab = nullptr;
    }
    free_gc_stack_roots(xdata.gc_stack_roots);
    ncc_template_registry_free(&tmpl_reg);
    ncc_xform_registry_free(&xreg);

    if (!comptime_vars_ok || !static_inits_ok) {
        ncc_free_comptime_vars(comptime_vars, n_comptime_vars);
        ncc_free_static_inits(static_inits, n_static_inits);
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    // Stage 7: Emit transformed C.
    ncc_pprint_opts_t pp_opts = {
        .line_width       = 100,
        .indent_size      = 4,
        .indent_style     = NCC_PPRINT_SPACES,
        .use_unicode_width = false,
        .out              = nullptr,
        .newline          = "\n",
        .style            = nullptr,
    };

    ncc_string_t emitted = ncc_pprint(g, tree, pp_opts);

    if (!emitted.data) {
        fprintf(stderr, "ncc: emission produced no output\n");
        ncc_free_comptime_vars(comptime_vars, n_comptime_vars);
        ncc_free_static_inits(static_inits, n_static_inits);
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    // Append the GC pointer-map descriptors (valid C at TU end: the element
    // types were typehash'd here and their definitions appear above).
    if (gc_typemap_text && gc_typemap_text[0]) {
        size_t base = emitted.u8_bytes;
        size_t add  = strlen(gc_typemap_text);
        char  *combined = (char *)malloc(base + add + 2);
        memcpy(combined, emitted.data, base);
        combined[base] = '\n';
        memcpy(combined + base + 1, gc_typemap_text, add);
        combined[base + 1 + add] = '\0';
        emitted.data     = combined;
        emitted.u8_bytes = base + 1 + add;
    }

    int rc = 0;
    const char *comptime_meta_text = ncc_ct_emit_section_decl_ex(
        opts, has_comptime_main ? &comptime_sig : nullptr,
        comptime_main_flags, comptime_vars, n_comptime_vars,
        static_inits, n_static_inits);
    if (!comptime_meta_text
        && (has_comptime_main || n_comptime_vars > 0
            || n_static_inits > 0)) {
        fprintf(stderr, "ncc: failed to emit comptime metadata section\n");
        ncc_free_comptime_vars(comptime_vars, n_comptime_vars);
        ncc_free_static_inits(static_inits, n_static_inits);
        rc = 1;
        goto cleanup;
    }
    if (comptime_meta_text) {
        (void)ncc_append_fragment(&emitted, comptime_meta_text);
        ncc_free((void *)comptime_meta_text);
    }
    ncc_free_comptime_vars(comptime_vars, n_comptime_vars);
    ncc_free_static_inits(static_inits, n_static_inits);

    size_t emit_len = emitted.u8_bytes;

    ncc_verbose("emitted %zu bytes", emit_len);

#ifdef NCC_MEM_DEBUG
    ncc_mem_report("after emit");
#endif

    if (opts->dump_output) {
        fprintf(stderr, "=== NCC EMITTED OUTPUT ===\n");
        fwrite(emitted.data, 1, emit_len, stderr);
        fprintf(stderr, "\n=== END NCC OUTPUT ===\n");
    }

    // Stage 8: Route output.
    if (opts->has_E) {
        ncc_verbose("emit mode target=%s",
                    opts->output_file ? opts->output_file : "stdout");
        // -E mode: emit to stdout or -o file.
        FILE *out = stdout;
        if (opts->output_file) {
            out = fopen(opts->output_file, "w");
            if (!out) {
                fprintf(stderr, "ncc: cannot open %s: %s\n",
                        opts->output_file, strerror(errno));
                rc = 1;
                goto cleanup;
            }
        }
        fwrite(emitted.data, 1, emit_len, out);
        if (out != stdout) {
            fclose(out);
        }
    }
    else {
        // Generate dependency file if requested (separate process on
        // original source, before compilation).
        if (opts->has_dep_flags) {
            ncc_verbose("generating depfile");
            int dep_rc = generate_depfile(opts);
            if (dep_rc != 0) {
                fprintf(stderr, "ncc: depfile generation failed (exit %d)\n",
                        dep_rc);
                rc = dep_rc;
                goto cleanup;
            }
        }

        // Compile: pipe transformed C to compiler.
        ncc_verbose("handing transformed source to compiler");
        if (ncc_is_link_invocation(opts)) {
            ncc_link_inputs_t inputs = {0};
            ncc_ct_rec_list_t records = {0};
            ncc_ct_aggregate_t agg = {0};
            char *err = nullptr;

            ncc_collect_link_inputs_from_clang_args(opts, &inputs);

            ncc_ct_aggregate_t source_guard_agg = {
                .has_comptime_main = has_comptime_main,
                .main_flags        = comptime_main_flags,
            };
            if (has_comptime_main
                && !ncc_comptime_guard_check(opts, &source_guard_agg, &err)) {
                print_process_message("comptime guard failed", err);
                rc = 1;
            }
            else if (!ncc_read_comptime_metadata_inputs(opts,
                                                        inputs.metadata_inputs,
                                                        inputs.n_metadata_inputs,
                                                        &records, &err)) {
                print_process_message("failed to read comptime metadata", err);
                rc = 1;
            }
            else if (has_comptime_main || n_comptime_vars > 0
                     || n_static_inits > 0
                     || records.n_records > 0) {
                ncc_temp_workspace_t ct_workspace = {0};
                char *tmp_err = nullptr;
                char *source_object = nullptr;

                if (!ncc_temp_workspace_create(&ct_workspace, "ncc_ct_src_",
                                               &tmp_err)) {
                    print_process_message("failed to create comptime source temp path",
                                          tmp_err);
                    ncc_free(tmp_err);
                    rc = 1;
                }
                else {
                    source_object = ncc_temp_workspace_join(&ct_workspace,
                                                            "source.o");
                    if (!source_object) {
                        fprintf(stderr,
                                "ncc: failed to create comptime source object path\n");
                        rc = 1;
                    }
                    else if (!ncc_compile_transformed_object(opts, emitted.data,
                                                             emit_len,
                                                             source_object)) {
                        rc = 1;
                    }
                    else {
                        ncc_push_const_arg(&inputs.user_inputs,
                                           &inputs.n_user_inputs,
                                           source_object);
                        ncc_insert_const_arg(&inputs.ordered_link_args,
                                             &inputs.n_ordered_link_args,
                                             0, source_object);
                        if (has_comptime_main || n_comptime_vars > 0
                            || n_static_inits > 0) {
                            ncc_push_const_arg(&inputs.metadata_inputs,
                                               &inputs.n_metadata_inputs,
                                               source_object);
                            if (!ncc_ct_read_object(opts, source_object,
                                                    &records, &err)) {
                                print_process_message("failed to read comptime metadata",
                                                      err);
                                rc = 1;
                            }
                        }
                    }
                }

                if (rc == 0) {
                    if (!ncc_aggregate_comptime_records(&records, &agg, &err)) {
                        print_process_message("failed to aggregate comptime metadata",
                                              err);
                        rc = 1;
                    }
                    else if (!ncc_comptime_guard_check(opts, &agg, &err)) {
                        print_process_message("comptime guard failed", err);
                        rc = 1;
                    }
                    else {
                        bool has_comptime_work = agg.has_comptime_main
                                                 || agg.n_static_inits > 0;
                        ncc_comptime_degrade_route_t degrade_route =
                            ncc_comptime_degrade_route(opts, &agg);

                        if ((degrade_route.static_init_degrade
                             || degrade_route.comptime_main_degrade)
                            && agg.n_static_inits > 0
                            && !ncc_static_init_degrade_allowed(&agg, &err)) {
                            print_process_message("static-init degrade failed",
                                                  err);
                            rc = 1;
                        }
                        else if (degrade_route.comptime_main_degrade
                                 || degrade_route.static_init_degrade) {
                            rc = ncc_degrade_comptime_link_plan(opts, &inputs,
                                                                &agg);
                        }
                        else if (!has_comptime_work || opts->no_comptime) {
                            rc = pipe_to_compiler(opts, emitted.data, emit_len);
                            if (rc == 0) {
                                rc = ncc_strip_comptime_output(opts);
                            }
                        }
                        else {
                            rc = ncc_run_comptime_link_plan(opts, &inputs,
                                                            &agg);
                        }
                    }
                }

                ncc_free(source_object);
                ncc_temp_workspace_cleanup(&ct_workspace);
            }
            else {
                rc = pipe_to_compiler(opts, emitted.data, emit_len);
            }

            ncc_free(err);
            ncc_ct_aggregate_free(&agg);
            ncc_ct_rec_list_free(&records);
            ncc_link_inputs_free(&inputs);
        }
        else {
            rc = pipe_to_compiler(opts, emitted.data, emit_len);
        }

        if (rc == 0 && opts->no_comptime && ncc_is_link_invocation(opts)) {
            rc = ncc_strip_comptime_output(opts);
        }
    }

cleanup:
    ncc_free(emitted.data);
    ncc_pwz_free(parser);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_grammar_free(g);

    // --ncc-error-on-union: a traditional union was found and reported at error
    // severity above; fail the compile (without disturbing a real error rc).
    if (union_error && rc == 0) {
        rc = 1;
    }
    return rc;
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    signal_setup();

    ncc_opts_t opts;
    parse_argv(&opts, argc, (const char **)argv);

    if (opts.ncc_help) {
        print_help();
        return 0;
    }

    if (opts.no_ncc) {
        return compiler_passthrough(&opts, argc, (const char **)argv);
    }

    if (opts.has_dep_only) {
        return compiler_passthrough(&opts, argc, (const char **)argv);
    }

    if (opts.gcmap_emit_out) {
        // Standalone aggregation: read n00b_gcraw from the link-input objects /
        // archives on the command line, generate+compile the typed dictionary
        // TU, and write it to the requested output path. No link is performed.
        // For build systems where ncc does not drive the final executable link.
        const char **objs   = ncc_alloc_array(const char *, argc);
        int          n_objs = 0;
        for (int i = 1; i < argc; i++) {
            if (is_linker_input(argv[i])) {
                objs[n_objs++] = argv[i];
            }
        }
        char *gc_err = nullptr;
        bool  ok     = ncc_gcmap_emit_to_path(&opts, objs, n_objs,
                                          opts.gcmap_emit_out, &gc_err);
        ncc_free(objs);
        if (!ok) {
            print_process_message("gcmap-emit failed", gc_err);
            ncc_free(gc_err);
            return 1;
        }
        return 0;
    }

    if (!opts.input_file) {
        // gcmap-prelink: aggregate the n00b_gcraw records from the link inputs
        // into one generated dictionary object and add it to the link. Done at
        // this single chokepoint so it covers every link path below. Gated on
        // the flag; the n00b build does not set it until the runtime reads the
        // dictionary (until then raw mode is GC-safe — conservative fallback).
        int          link_argc = argc;
        const char **link_argv = (const char **)argv;
        ncc_temp_workspace_t gc_tmp = {0};
        if (opts.gcmap_prelink) {
            const char **objs   = ncc_alloc_array(const char *, argc);
            int          n_objs = 0;
            for (int i = 1; i < argc; i++) {
                if (is_linker_input(argv[i])) {
                    objs[n_objs++] = argv[i];
                }
            }
            char *gc_obj = nullptr;
            char *gc_err = nullptr;
            char *tmp_err = nullptr;
            if (!ncc_temp_workspace_create(&gc_tmp, "ncc_gcmap_link_", &tmp_err)) {
                print_process_message("gcmap-prelink temp workspace failed",
                                      tmp_err);
                return 1;
            }
            if (!ncc_gcmap_prelink_build_object(&opts, objs, n_objs, &gc_tmp,
                                                &gc_obj, &gc_err)) {
                print_process_message("gcmap-prelink failed", gc_err);
                return 1;
            }
            ncc_free(objs);
            if (gc_obj) {
                link_argv = ncc_alloc_array(const char *, (size_t)argc + 2);
                for (int i = 0; i < argc; i++) {
                    link_argv[i] = (const char *)argv[i];
                }
                link_argv[argc]     = gc_obj;
                link_argv[argc + 1] = nullptr;
                link_argc           = argc + 1;
            }
        }

        bool comptime_handled = false;
        int  comptime_rc = maybe_comptime_link_passthrough(&opts, link_argc,
                                                          link_argv,
                                                          &comptime_handled);
        if (comptime_handled) {
            return comptime_rc;
        }
        if (opts.custom_entry && !opts.has_E && !opts.has_c && !opts.has_S
            && !opts.has_fsyntax_only) {
            return custom_entry_link_passthrough(&opts, link_argc, link_argv);
        }
        // No source file — passthrough to compiler (linking, etc.).
        return compiler_passthrough(&opts, link_argc, link_argv);
    }

    return compile_file(&opts);
}
