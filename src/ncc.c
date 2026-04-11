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
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <execinfo.h>
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <execinfo.h>
#include <limits.h>
#endif

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/typedef_walk.h"
#include "parse/c_tokenizer.h"
#include "parse/emit.h"
#include "parse/tree_dump.h"
#include "xform/transform.h"
#include "xform/xform_template.h"
#include "parse/symtab.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"

static const char embedded_grammar[] = {
#embed "c_ncc.bnf"
, '\0'
};
static const size_t embedded_grammar_len = sizeof(embedded_grammar) - 1;

// Transform registration prototypes.
extern void ncc_register_generic_struct_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typeid_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typestr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_typehash_xform(ncc_xform_registry_t *reg);
extern void ncc_register_once_xform(ncc_xform_registry_t *reg);
extern void ncc_register_bang_xform(ncc_xform_registry_t *reg);
extern void ncc_register_rstr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_constexpr_xform(ncc_xform_registry_t *reg);
extern void ncc_register_constexpr_paste_xform(ncc_xform_registry_t *reg);
extern void ncc_register_kargs_vargs_xform(ncc_xform_registry_t *reg);
extern void ncc_register_option_xform(ncc_xform_registry_t *reg);
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
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    _exit(1);
}

static void
signal_setup(void)
{
    static char     altstack[65536];
    static stack_t  ss = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
    struct sigaction sa = {.sa_handler = crash_handler, .sa_flags = SA_ONSTACK};

    sigaltstack(&ss, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

// ============================================================================
// Verbose logging
// ============================================================================

static bool verbose = false;

#define ncc_verbose(fmt, ...) \
    do { if (verbose) fprintf(stderr, "ncc: " fmt "\n" __VA_OPT__(,) __VA_ARGS__); } while (0)

// ============================================================================
// Parsed command-line state
// ============================================================================

typedef struct {
    const char  *input_file;
    const char  *output_file;
    const char  *compiler;
    const char  *constexpr_headers; // comma-separated <...> or "..." headers

    bool         has_E;
    bool         has_c;
    bool         has_std;       // user specified -std=
    bool         no_ncc;
    bool         ncc_help;
    bool         dump_tokens;
    bool         dump_tree;
    bool         dump_tree_raw;
    bool         dump_output;

    // Dependency file generation (handled separately from compilation).
    bool         has_dep_flags; // -MD or -MMD present
    bool         dep_mmmd;      // -MMD (user headers only) vs -MD (all)
    const char  *dep_mf;        // -MF <file>
    const char  *dep_mq;        // -MQ <target>
    const char  *dep_mt;        // -MT <target>

    // rstr template overrides (CLI > meson define > default).
    const char  *rstr_template_styled;
    const char  *rstr_template_plain;

    // vargs/once/rstr overrides (CLI > meson define > default).
    const char  *vargs_type;
    const char  *once_prefix;
    const char  *rstr_string_type;

    // Grammar file override (CLI > env > embedded).
    const char  *grammar_file;

    // Flags to pass through to clang.
    const char **clang_args;
    int          n_clang_args;
    int          clang_args_cap;
} ncc_opts_t;

#include "xform/xform_data.h"

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

// Forward declaration — defined below.
static char *get_exe_path(void);

// ============================================================================
// Self-recursion guard: detect if a path resolves to our own executable
// ============================================================================

static bool
is_ncc_path(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

    char *our_exe = get_exe_path();
    if (!our_exe) {
        return false;
    }

    char *candidate = realpath(path, nullptr);
    if (!candidate) {
        ncc_free(our_exe);
        return false;
    }

    bool same = (strcmp(our_exe, candidate) == 0);
    ncc_free(our_exe);
    ncc_free(candidate);
    return same;
}

// ============================================================================
// Argument parsing (simple hand-rolled, no commander dependency)
// ============================================================================

static void
parse_argv(ncc_opts_t *opts, int argc, const char **argv)
{
    memset(opts, 0, sizeof(*opts));

    opts->compiler = getenv("NCC_COMPILER");
    if (!opts->compiler) {
        const char *cc_env = getenv("CC");
        if (cc_env && !is_ncc_path(cc_env)) {
            opts->compiler = cc_env;
        }
    }
#ifdef NCC_DEFAULT_CC
    if (!opts->compiler) {
        opts->compiler = NCC_DEFAULT_CC;
    }
#endif
    if (!opts->compiler) {
        opts->compiler = "/usr/local/bin/clang";
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

    if (getenv("NCC_VERBOSE")) {
        verbose = true;
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
        "  --ncc-constexpr-include HDRS\n"
        "                       Comma-separated headers for constexpr eval\n"
        "                       (e.g. '<myheader.h>,\"local.h\"')\n"
        "                       Overrides NCC_CONSTEXPR_HEADERS env var\n"
        "  --ncc-grammar FILE   Use external grammar file instead of built-in\n"
        "                       Overrides NCC_GRAMMAR env var\n"
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
#ifdef __APPLE__
    uint32_t sz = 0;
    _NSGetExecutablePath(nullptr, &sz);

    char *buf = ncc_alloc_size(1, sz + 1);
    if (_NSGetExecutablePath(buf, &sz) != 0) {
        ncc_free(buf);
        return nullptr;
    }

    char *resolved = realpath(buf, nullptr);
    ncc_free(buf);
    return resolved;
#elif defined(__linux__)
    char *resolved = realpath("/proc/self/exe", nullptr);
    return resolved;
#else
    return nullptr;
#endif
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
// Pre-CPP scan: r"..." → __ncc_rstr("...")
// ============================================================================

static bool
is_id_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Rewrite r"..." to __ncc_rstr("...") in a source buffer.
 *
 * Scans the raw source before the C preprocessor runs. Tracks comment
 * and string context to avoid false matches. Handles adjacent string
 * concatenation (r"a" "b" → __ncc_rstr("a" "b")).
 *
 * Returns a new buffer if any r"..." were found (caller frees old),
 * or the original buffer unchanged.
 */
static char *
rstr_prescan(char *src, size_t len, size_t *out_len)
{
    // Quick check: does the buffer contain r" at all?
    bool has_rstr = false;

    for (size_t i = 0; i + 1 < len; i++) {
        if (src[i] == 'r' && src[i + 1] == '"') {
            if (i == 0 || !is_id_char(src[i - 1])) {
                has_rstr = true;
                break;
            }
        }
    }

    if (!has_rstr) {
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
            // r"..." match?
            if (src[i] == 'r' && i + 1 < len && src[i + 1] == '"') {
                if (i == 0 || !is_id_char(src[i - 1])) {
                    // Flush everything before the 'r'.
                    if (i > flush_from) {
                        ncc_buffer_append(buf, src + flush_from, i - flush_from);
                    }
                    // Emit __ncc_rstr( instead of r.
                    ncc_buffer_append(buf, "__ncc_rstr(", 11);
                    i++; // skip the 'r', keep the '"'

                    // Scan forward to find the end of the string.
                    size_t str_start = i;
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

                    // Check for adjacent string concatenation.
                    while (i < len) {
                        size_t ws = i;

                        while (ws < len
                               && (src[ws] == ' ' || src[ws] == '\t'
                                   || src[ws] == '\n' || src[ws] == '\r')) {
                            ws++;
                        }

                        if (ws < len && src[ws] == '"') {
                            i = ws + 1;

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
                        }
                        else {
                            break;
                        }
                    }

                    // Emit the string content and closing ).
                    ncc_buffer_append(buf, src + str_start, i - str_start);
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

    // Prescan: rewrite r"..." → __ncc_rstr("...") before CPP.
    // This must happen on the raw source before preprocessing.
    char  *wrapped     = rstr_prescan(raw_src, raw_len, &raw_len);
    size_t wrapped_len = raw_len;

    // Prepend a line marker so CPP attributes output to the original file.
    // This makes compiler error messages reference the real source.
    char line_marker[2048];
    int  marker_len = snprintf(line_marker, sizeof(line_marker),
                                "# 1 \"%s\"\n", input_file);

    size_t total_len = (size_t)marker_len + wrapped_len;
    char  *with_marker = ncc_alloc_size(1, total_len + 1);
    memcpy(with_marker, line_marker, (size_t)marker_len);
    memcpy(with_marker + marker_len, wrapped, wrapped_len);
    with_marker[total_len] = '\0';
    ncc_free(wrapped);
    wrapped     = with_marker;
    wrapped_len = total_len;

    // Write wrapped source to a temp file.
    char tmp_path[] = "/tmp/ncc_XXXXXX.c";
    int  tmp_fd     = mkstemps(tmp_path, 2);

    if (tmp_fd < 0) {
        fprintf(stderr, "ncc: mkstemps() failed: %s\n", strerror(errno));
        ncc_free(wrapped);
        return nullptr;
    }

    size_t  w_written = 0;

    while (w_written < wrapped_len) {
        ssize_t n = write(tmp_fd, wrapped + w_written,
                          wrapped_len - w_written);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "ncc: write to temp file: %s\n", strerror(errno));
            close(tmp_fd);
            unlink(tmp_path);
            ncc_free(wrapped);
            return nullptr;
        }

        w_written += (size_t)n;
    }

    close(tmp_fd);
    ncc_free(wrapped);

    // Extract source directory for -I so relative includes still resolve.
    char       *src_dir     = strdup(input_file);
    char       *last_slash  = strrchr(src_dir, '/');
    const char *include_dir = nullptr;

    if (last_slash) {
        *last_slash = '\0';
        include_dir = src_dir;
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

    if (verbose) {
        fprintf(stderr, "ncc: running:");
        for (int i = 0; argv[i]; i++) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }

    int pipefd[2];

    if (pipe(pipefd) < 0) {
        fprintf(stderr, "ncc: pipe() failed: %s\n", strerror(errno));
        ncc_free(argv);
        ncc_free(src_dir);
        unlink(tmp_path);
        return nullptr;
    }

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "ncc: fork() failed: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        ncc_free(argv);
        ncc_free(src_dir);
        unlink(tmp_path);
        return nullptr;
    }

    if (pid == 0) {
        // Child: redirect stdout to pipe, inherit stderr.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(compiler, (char *const *)argv);
        _exit(127);
    }

    // Parent: read from pipe.
    close(pipefd[1]);
    ncc_free(argv);

    ncc_buffer_t *rbuf = ncc_buffer_empty();
    ncc_buffer_ensure(rbuf, 1 << 16);

    for (;;) {
        ncc_buffer_ensure(rbuf, 4096);

        ssize_t n = read(pipefd[0], rbuf->data + rbuf->byte_len, 4096);

        if (n <= 0) {
            break;
        }

        rbuf->byte_len += (size_t)n;
    }

    // NUL-terminate for downstream string use.
    ncc_buffer_putc(rbuf, '\0');
    rbuf->byte_len--;

    close(pipefd[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    // Clean up temp file.
    unlink(tmp_path);
    ncc_free(src_dir);

    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        int code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
        fprintf(stderr, "ncc: preprocessor failed (exit %d)\n", code);
        ncc_buffer_free(rbuf);
        return nullptr;
    }

    size_t len = rbuf->byte_len;
    char  *buf = ncc_buffer_take(rbuf);

    // Post-process: wrap system header regions with #pragma ncc off/on.
    // CPP line markers with flag 3 indicate system headers.
    size_t wrapped_pp_len = 0;
    char  *wrapped_pp     = wrap_system_headers(buf, len, &wrapped_pp_len);
    ncc_free(buf);

    if (out_len) {
        *out_len = wrapped_pp_len;
    }

    return wrapped_pp;
}

// ============================================================================
// Compiler passthrough (exec clang with all original args)
// ============================================================================

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

        // Skip ncc-specific flags.
        if (strncmp(arg, "--ncc-", 6) == 0 || strcmp(arg, "--no-ncc") == 0) {
            continue;
        }
        new_argv[n++] = arg;
    }
    new_argv[n] = nullptr;

    execvp(opts->compiler, (char *const *)new_argv);
    fprintf(stderr, "ncc: failed to exec %s: %s\n", opts->compiler,
            strerror(errno));
    ncc_free(new_argv);
    return 1;
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

static int
pipe_to_compiler(const ncc_opts_t *opts, const char *c_source, size_t c_len)
{
    // Build argv: compiler [-std=gnu23] [flags...] -x c - [-x none linker-inputs...]
    // clang_args already includes -c, -o, and all passthrough flags.
    // Linker inputs (.a, .o, .so) must come after "-x c -" with a
    // "-x none" reset so clang treats them correctly on all platforms.
    int          max_args = 10 + opts->n_clang_args * 2;
    const char **argv     = ncc_alloc_array(const char *, (size_t)(max_args + 1));
    int          ai       = 0;

    argv[ai++] = opts->compiler;

    if (!opts->has_std) {
        argv[ai++] = "-std=gnu23";
    }

    // Suppress ODR warnings from alignas attributes in transformed code.
    argv[ai++] = "-Wno-odr";

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
        else if (!is_linker_input(opts->clang_args[i])) {
            argv[ai++] = opts->clang_args[i];
        }
    }

    argv[ai++] = "-x";
    argv[ai++] = "c";
    argv[ai++] = "-";

    // Append linker inputs (.a, .o, etc.) after "-x c -", resetting
    // the language so clang doesn't misinterpret them as C source.
    bool need_reset = true;
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

    if (verbose) {
        fprintf(stderr, "ncc: compiling:");
        for (int i = 0; argv[i]; i++) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }

    int pipefd[2];

    if (pipe(pipefd) < 0) {
        fprintf(stderr, "ncc: pipe() failed: %s\n", strerror(errno));
        ncc_free(argv);
        return 1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "ncc: fork() failed: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        ncc_free(argv);
        return 1;
    }

    if (pid == 0) {
        // Child: read transformed C from pipe stdin.
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        execvp(opts->compiler, (char *const *)argv);
        _exit(127);
    }

    // Parent: write transformed C to pipe, then close.
    close(pipefd[0]);
    ncc_free(argv);

    size_t  written = 0;

    while (written < c_len) {
        ssize_t n = write(pipefd[1], c_source + written, c_len - written);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "ncc: write to compiler pipe: %s\n",
                    strerror(errno));
            break;
        }

        written += (size_t)n;
    }

    close(pipefd[1]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }

    return 1;
}

// ============================================================================
// Dependency file generation
// ============================================================================
//
// When -MD or -MMD is present, run a separate clang -M process on the
// original source file to capture real #include dependencies.  This must
// use the original source (not our transformed stdin) so the dependency
// file lists actual header paths.

static int
generate_depfile(const ncc_opts_t *opts)
{
    if (!opts->has_dep_flags) {
        return 0;
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

    if (opts->dep_mf) {
        argv[ai++] = "-MF";
        argv[ai++] = opts->dep_mf;
    }
    if (opts->dep_mq) {
        argv[ai++] = "-MQ";
        argv[ai++] = opts->dep_mq;
    }
    if (opts->dep_mt) {
        argv[ai++] = "-MT";
        argv[ai++] = opts->dep_mt;
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

    if (verbose) {
        fprintf(stderr, "ncc: depfile:");
        for (int i = 0; argv[i]; i++) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "ncc: fork() for depfile failed: %s\n",
                strerror(errno));
        ncc_free(argv);
        return 1;
    }

    if (pid == 0) {
        execvp(opts->compiler, (char *const *)argv);
        _exit(127);
    }

    ncc_free(argv);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }

    return 1;
}

// ============================================================================
// Main pipeline: preprocess → tokenize → parse → transform → emit
// ============================================================================

static int
compile_file(ncc_opts_t *opts)
{
    // Stage 1: Load grammar.
    ncc_grammar_t *g = load_c_grammar(opts);
    if (!g) {
        return 1;
    }

    // Stage 2: Preprocess with clang -E.
    size_t pp_len  = 0;
    char  *pp_text = run_preprocessor(opts, &pp_len);
    if (!pp_text) {
        ncc_grammar_free(g);
        return 1;
    }

    ncc_verbose("preprocessed %zu bytes", pp_len);

    // Stage 3: Tokenize.
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

    // Stage 5: Reclassify walk (typedef tracking).
    int32_t reclassified = ncc_typedef_walk(
        g, tree, ts->tokens, ts->token_count);

    if (reclassified > 0) {
        ncc_verbose("reclassified %d tokens", reclassified);
    }

    // Stage 5.5: Dump pre-transform tree.
    if (opts->dump_tree) {
        ncc_tree_dump(g, tree, stderr, opts->dump_tree_raw);
    }

    // Stage 6: Transform passes (typeid, typestr, typehash).
    ncc_xform_registry_t xreg;
    ncc_xform_registry_init(&xreg, g);
    ncc_register_generic_struct_xform(&xreg);
    ncc_register_typeid_xform(&xreg);
    ncc_register_option_xform(&xreg);
    ncc_register_typestr_xform(&xreg);
    ncc_register_typehash_xform(&xreg);
    ncc_register_kargs_vargs_xform(&xreg);
    ncc_register_once_xform(&xreg);
    ncc_register_bang_xform(&xreg);
    ncc_register_rstr_xform(&xreg);
    ncc_register_constexpr_xform(&xreg);
    ncc_register_constexpr_paste_xform(&xreg);

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

    // Resolution order: CLI flag > meson define > built-in default.
    const char *rstr_styled = default_rstr_styled;
    const char *rstr_plain  = default_rstr_plain;

#ifdef NCC_RSTR_TEMPLATE_STYLED
    rstr_styled = NCC_RSTR_TEMPLATE_STYLED;
#endif
#ifdef NCC_RSTR_TEMPLATE_PLAIN
    rstr_plain = NCC_RSTR_TEMPLATE_PLAIN;
#endif

    if (opts->rstr_template_styled) {
        rstr_styled = opts->rstr_template_styled;
    }
    if (opts->rstr_template_plain) {
        rstr_plain = opts->rstr_template_plain;
    }

    ncc_template_register(&tmpl_reg, "rstr_styled",
                           "primary_expression", rstr_styled);
    ncc_template_register(&tmpl_reg, "rstr_plain",
                           "primary_expression", rstr_plain);

    // _Once templates (see templates/*.c.tmpl for slot documentation).
    static const char once_tmpl[] = {
#embed "templates/once.c.tmpl"
        , '\0'
    };
    static const char once_void_tmpl[] = {
#embed "templates/once_void.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "once", "translation_unit", once_tmpl);
    ncc_template_register(&tmpl_reg, "once_void", "translation_unit",
                           once_void_tmpl);

    // Bang (!) error propagation template.
    static const char bang_tmpl[] = {
#embed "templates/bang.c.tmpl"
        , '\0'
    };
    ncc_template_register(&tmpl_reg, "bang", "primary_expression", bang_tmpl);

    // Resolve vargs_type, once_prefix, rstr_string_type: CLI > meson define > default.
    const char *vargs_type      = "ncc_vargs_t";
    const char *once_prefix     = "__ncc_";
    const char *rstr_string_type = "ncc_string_t*";

#ifdef NCC_VARGS_TYPE
    vargs_type = NCC_VARGS_TYPE;
#endif
#ifdef NCC_ONCE_PREFIX
    once_prefix = NCC_ONCE_PREFIX;
#endif
#ifdef NCC_RSTR_STRING_TYPE
    rstr_string_type = NCC_RSTR_STRING_TYPE;
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

    ncc_xform_ctx_t xctx;
    ncc_xform_ctx_init(&xctx, g, &xreg, tree);
    ncc_xform_data_t xdata = {
        .compiler          = opts->compiler,
        .constexpr_headers = opts->constexpr_headers,
        .func_meta         = {0},
        .template_reg      = &tmpl_reg,
        .vargs_type        = vargs_type,
        .once_prefix       = once_prefix,
        .rstr_string_type  = rstr_string_type,
    };
    ncc_dict_init(&xdata.option_meta,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.option_decls,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    ncc_dict_init(&xdata.generic_struct_decls,
                            ncc_hash_cstring, ncc_dict_cstr_eq);
    xctx.user_data = &xdata;
    tree = ncc_xform_apply(&xreg, &xctx);

    if (xctx.nodes_replaced > 0) {
        ncc_verbose("transforms: %d nodes replaced", xctx.nodes_replaced);
    }

    ncc_dict_free(&xdata.option_meta);
    ncc_dict_free(&xdata.option_decls);
    ncc_dict_free(&xdata.generic_struct_decls);
    ncc_template_registry_free(&tmpl_reg);
    ncc_xform_registry_free(&xreg);

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
        ncc_pwz_free(parser);
        ncc_token_stream_free(ts);
        ncc_scanner_free(scanner);
        ncc_grammar_free(g);
        return 1;
    }

    size_t emit_len = emitted.u8_bytes;

    ncc_verbose("emitted %zu bytes", emit_len);

    if (opts->dump_output) {
        fprintf(stderr, "=== NCC EMITTED OUTPUT ===\n");
        fwrite(emitted.data, 1, emit_len, stderr);
        fprintf(stderr, "\n=== END NCC OUTPUT ===\n");
    }

    // Stage 8: Route output.
    int rc = 0;

    if (opts->has_E) {
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
            int dep_rc = generate_depfile(opts);
            if (dep_rc != 0) {
                ncc_verbose("depfile generation failed (rc=%d)", dep_rc);
            }
        }

        // Compile: pipe transformed C to compiler.
        rc = pipe_to_compiler(opts, emitted.data, emit_len);
    }

cleanup:
    ncc_free(emitted.data);
    ncc_pwz_free(parser);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_grammar_free(g);

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

    if (!opts.input_file) {
        // No source file — passthrough to compiler (linking, etc.).
        return compiler_passthrough(&opts, argc, (const char **)argv);
    }

    return compile_file(&opts);
}
