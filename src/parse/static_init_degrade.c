#include "parse/static_init_degrade.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void
set_err(char **err_out, const char *fmt, ...)
{
    if (!err_out) {
        return;
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;

    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);

    ncc_free(*err_out);
    *err_out = ncc_buffer_take(buf);
}

static bool
compile_arg_takes_value(const char *arg)
{
    return strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0
           || strcmp(arg, "-arch") == 0 || strcmp(arg, "-isysroot") == 0
           || strcmp(arg, "--sysroot") == 0 || strcmp(arg, "-march") == 0
           || strcmp(arg, "-mcpu") == 0 || strcmp(arg, "-mtune") == 0
           || strcmp(arg, "-mabi") == 0;
}

static bool
compile_arg_is_single(const char *arg)
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
append_compile_target_args(const ncc_opts_t *opts, const char **argv, int ai)
{
    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (compile_arg_takes_value(arg)) {
            argv[ai++] = arg;
            if (i + 1 < opts->n_clang_args) {
                argv[ai++] = opts->clang_args[++i];
            }
            continue;
        }

        if (compile_arg_is_single(arg)) {
            argv[ai++] = arg;
        }
    }

    return ai;
}

static char *
process_stderr_string(const ncc_process_result_t *proc)
{
    if (!proc || !proc->stderr_data || proc->stderr_len == 0) {
        return ncc_buffer_take(ncc_buffer_empty());
    }

    char *out = ncc_alloc_array(char, proc->stderr_len + 1);
    memcpy(out, proc->stderr_data, proc->stderr_len);
    return out;
}

static bool
ct_name_is_c_identifier(ncc_string_t name)
{
    if (!name.data || name.u8_bytes <= 0) {
        return false;
    }

    unsigned char first = (unsigned char)name.data[0];
    if (!(isalpha(first) || first == '_')) {
        return false;
    }

    for (size_t i = 1; i < name.u8_bytes; i++) {
        unsigned char ch = (unsigned char)name.data[i];
        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }

    return true;
}

static bool
validate_static_init_degrade_meta(const ncc_ct_aggregate_t *meta,
                                  char **err_out)
{
    if (!meta || meta->n_static_inits <= 0) {
        set_err(err_out, "no static initializers to degrade");
        return false;
    }

    for (int i = 0; i < meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &meta->static_inits[i];
        if (!ct_name_is_c_identifier(si->name)) {
            set_err(err_out,
                    "static initializer metadata has unsupported name '%.*s'",
                    (int)si->name.u8_bytes,
                    si->name.data ? si->name.data : "");
            return false;
        }
        if (si->degrade_ok == 0) {
            set_err(err_out,
                    "static initializer '%.*s' cannot be lowered to runtime "
                    "initialization for this target",
                    (int)si->name.u8_bytes, si->name.data);
            return false;
        }
    }

    return true;
}

static char *
generated_degrade_source(const ncc_ct_aggregate_t *meta)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-static-init-degrade.c\"\n"
        "typedef int (*n00b_static_init_fn_t)(void);\n"
        "#if defined(__APPLE__)\n"
        "#define NCC_STATIC_INIT_ENTRY(sym, fn) \\\n"
        "    [[gnu::used]] [[gnu::retain]] \\\n"
        "    [[gnu::section(\"" NCC_STATIC_INIT_DEGRADE_SECTION_MACHO "\")]] \\\n"
        "    static n00b_static_init_fn_t const sym = fn\n"
        "#elif defined(_WIN32)\n"
        "#define NCC_STATIC_INIT_ENTRY(sym, fn) \\\n"
        "    [[gnu::used]] [[gnu::section(\"" NCC_STATIC_INIT_DEGRADE_SECTION_PE "\")]] \\\n"
        "    static n00b_static_init_fn_t const sym = fn\n"
        "#else\n"
        "#define NCC_STATIC_INIT_ENTRY(sym, fn) \\\n"
        "    [[gnu::used]] [[gnu::retain]] \\\n"
        "    [[gnu::section(\"" NCC_STATIC_INIT_DEGRADE_SECTION_ELF "\")]] \\\n"
        "    static n00b_static_init_fn_t const sym = fn\n"
        "#endif\n");

    for (int i = 0; i < meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &meta->static_inits[i];
        ncc_buffer_printf(buf,
            "extern int __ncc_static_init_prepare_%.*s(void);\n"
            "static int __ncc_static_init_degrade_%.*s(void)\n"
            "{\n"
            "    return __ncc_static_init_prepare_%.*s();\n"
            "}\n"
            "NCC_STATIC_INIT_ENTRY(__ncc_static_init_degrade_entry_%.*s,\n"
            "                      __ncc_static_init_degrade_%.*s);\n",
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data);
    }

    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-static-init-degrade-end.c\"\n");
    return ncc_buffer_take(buf);
}

bool
ncc_emit_static_init_degrade_object(const ncc_opts_t *opts,
                                    const ncc_ct_aggregate_t *meta,
                                    const char *out_obj_path,
                                    char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }
    if (!opts || !opts->compiler || !out_obj_path) {
        set_err(err_out, "invalid static-init degrade emit request");
        return false;
    }
    if (!validate_static_init_degrade_meta(meta, err_out)) {
        return false;
    }

    char *source = generated_degrade_source(meta);
    ncc_buffer_t *source_path_buf = ncc_buffer_empty();
    ncc_buffer_printf(source_path_buf, "%s.c", out_obj_path);
    char *source_path = ncc_buffer_take(source_path_buf);

    char *write_err = nullptr;
    if (!ncc_platform_write_file(source_path, source, strlen(source),
                                 &write_err)) {
        set_err(err_out, "write to static-init degrade temp file failed: %s",
                write_err ? write_err : "(no detail)");
        ncc_free(write_err);
        ncc_free(source);
        ncc_free(source_path);
        return false;
    }
    ncc_free(source);

    int max_args = 8 + (opts ? opts->n_clang_args : 0);
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args + 1);
    int ai = 0;

    argv[ai++] = opts->compiler;
    ai = append_compile_target_args(opts, argv, ai);
    argv[ai++] = "-std=gnu23";
    argv[ai++] = "-c";
    argv[ai++] = source_path;
    argv[ai++] = "-o";
    argv[ai++] = out_obj_path;
    argv[ai] = nullptr;

    ncc_process_spec_t spec = {
        .program        = opts->compiler,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok = launched && proc.exit_code == 0;

    if (!ok) {
        char *stderr_text = process_stderr_string(&proc);
        set_err(err_out, "static-init degrade object compile failed: %s",
                stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_platform_remove_file(source_path);
    ncc_free(source_path);
    return ok;
}
