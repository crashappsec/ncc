#include "parse/comptime_image_emit.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NCC_CT_IMAGE_PROTECT_ALIGN 65536u

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

static char *
read_binary_file(const char *path, size_t *len_out, char **err_out)
{
    if (len_out) {
        *len_out = 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err_out, "open failed for '%s': %s",
                path ? path : "(null)", strerror(errno));
        return nullptr;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        set_err(err_out, "seek failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    long end = ftell(f);
    if (end < 0) {
        set_err(err_out, "tell failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        set_err(err_out, "seek failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    char  *data = ncc_alloc_array(char, (size_t)end + 1);
    size_t got  = fread(data, 1, (size_t)end, f);
    int    rc   = fclose(f);

    if (got != (size_t)end || rc != 0) {
        set_err(err_out, "read failed for '%s': %s", path, strerror(errno));
        ncc_free(data);
        return nullptr;
    }

    if (len_out) {
        *len_out = (size_t)end;
    }
    return data;
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

static char *
generated_image_source(const char *bytes, size_t len, bool writable)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    size_t protect_len = (len + NCC_CT_IMAGE_PROTECT_ALIGN - 1u)
                       & ~(NCC_CT_IMAGE_PROTECT_ALIGN - 1u);
    size_t stored_len = writable ? len : protect_len;
    const char *elf_section = writable ? NCC_CT_WRITABLE_IMAGE_SECTION_ELF
                                       : NCC_CT_IMAGE_SECTION_ELF;
    const char *macho_section = writable ? NCC_CT_WRITABLE_IMAGE_SECTION_MACHO
                                         : NCC_CT_IMAGE_SECTION_MACHO;
    const char *pe_section = writable ? NCC_CT_WRITABLE_IMAGE_SECTION_PE
                                      : NCC_CT_IMAGE_SECTION_PE;
    const char *symbol = writable ? "__n00b_ct_writable_image"
                                  : "__n00b_ct_image";
    const char *len_symbol = writable ? "__n00b_ct_writable_image_len"
                                      : "__n00b_ct_image_len";

    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-comptime-image.c\"\n"
        "#if defined(_WIN32)\n"
        "#define NCC_CT_IMAGE_VIS\n"
        "#else\n"
        "#define NCC_CT_IMAGE_VIS [[gnu::visibility(\"hidden\")]]\n"
        "#endif\n"
        "#if defined(__APPLE__)\n"
        "[[gnu::used]] [[gnu::retain]] NCC_CT_IMAGE_VIS [[gnu::aligned(65536)]] ");
    ncc_buffer_printf(buf, "[[gnu::section(\"%s\")]]\n", macho_section);
    ncc_buffer_puts(buf,
        "#elif defined(_WIN32)\n"
        "[[gnu::used]] [[gnu::retain]] NCC_CT_IMAGE_VIS [[gnu::aligned(65536)]] ");
    ncc_buffer_printf(buf, "[[gnu::section(\"%s\")]]\n", pe_section);
    ncc_buffer_puts(buf,
        "#else\n"
        "[[gnu::used]] [[gnu::retain]] NCC_CT_IMAGE_VIS [[gnu::aligned(65536)]] ");
    ncc_buffer_printf(buf, "[[gnu::section(\"%s\")]]\n", elf_section);
    ncc_buffer_puts(buf,
        "#endif\n"
        "unsigned char ");
    ncc_buffer_printf(buf, "%s[] = {\n", symbol);

    for (size_t i = 0; i < stored_len; i++) {
        if ((i % 12) == 0) {
            ncc_buffer_puts(buf, "\n");
        }
        unsigned char byte = i < len ? (unsigned char)bytes[i] : 0;
        ncc_buffer_printf(buf, "0x%02x,", byte);
    }

    ncc_buffer_printf(buf,
        "\n};\n"
        "NCC_CT_IMAGE_VIS const unsigned long long %s = %zuULL;\n",
        len_symbol, len);
    if (!writable) {
        ncc_buffer_printf(buf,
            "NCC_CT_IMAGE_VIS const unsigned long long "
            "__n00b_ct_image_protect_len = %zuULL;\n",
            protect_len);
    }
    ncc_buffer_puts(buf, "# line 1 \"ncc-generated-comptime-image-end.c\"\n");

    return ncc_buffer_take(buf);
}

static bool
emit_image_object(const ncc_opts_t *opts, const char *image_bytes_path,
                  const char *out_obj_path, bool writable, char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }
    if (!opts || !opts->compiler || !image_bytes_path || !out_obj_path) {
        set_err(err_out, "invalid comptime image emit request");
        return false;
    }

    size_t image_len = 0;
    char  *image = read_binary_file(image_bytes_path, &image_len, err_out);
    if (!image) {
        return false;
    }
    if (image_len == 0) {
        set_err(err_out, "comptime image is empty: %s", image_bytes_path);
        ncc_free(image);
        return false;
    }

    char *source = generated_image_source(image, image_len, writable);
    ncc_free(image);

    ncc_buffer_t *source_path_buf = ncc_buffer_empty();
    ncc_buffer_printf(source_path_buf, "%s.c", out_obj_path);
    char *source_path = ncc_buffer_take(source_path_buf);

    char *write_err = nullptr;
    if (!ncc_platform_write_file(source_path, source, strlen(source), &write_err)) {
        set_err(err_out, "write to comptime image temp file failed: %s",
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
        set_err(err_out, "comptime image object compile failed: %s", stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_platform_remove_file(source_path);
    ncc_free(source_path);
    return ok;
}

bool
ncc_emit_image_object(const ncc_opts_t *opts, const char *image_bytes_path,
                      const char *out_obj_path, char **err_out)
{
    return emit_image_object(opts, image_bytes_path, out_obj_path, false,
                             err_out);
}

bool
ncc_emit_writable_image_object(const ncc_opts_t *opts,
                               const char *image_bytes_path,
                               const char *out_obj_path, char **err_out)
{
    return emit_image_object(opts, image_bytes_path, out_obj_path, true,
                             err_out);
}
