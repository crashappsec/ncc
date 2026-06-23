#include "parse/comptime_build.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/comptime_guard.h"
#include "parse/crt_entry.h"
#include "parse/comptime_image_emit.h"
#include "parse/static_init_degrade.h"
#include "util/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef enum {
    NCC_CRT_TARGET_LINUX,
    NCC_CRT_TARGET_DARWIN,
    NCC_CRT_TARGET_WINDOWS,
} ncc_ct_target_os_t;

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
read_file_bytes(const char *path, size_t *len_out, char **err_out)
{
    if (len_out) {
        *len_out = 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err_out, "failed to open '%s'", path ? path : "(null)");
        return nullptr;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        set_err(err_out, "failed to seek '%s'", path);
        fclose(f);
        return nullptr;
    }

    long end = ftell(f);
    if (end < 0) {
        set_err(err_out, "failed to tell '%s'", path);
        fclose(f);
        return nullptr;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        set_err(err_out, "failed to rewind '%s'", path);
        fclose(f);
        return nullptr;
    }

    char *bytes = ncc_alloc_array(char, (size_t)end + 1);
    size_t got = fread(bytes, 1, (size_t)end, f);
    int close_rc = fclose(f);

    if (got != (size_t)end || close_rc != 0) {
        set_err(err_out, "failed to read '%s'", path);
        ncc_free(bytes);
        return nullptr;
    }

    if (len_out) {
        *len_out = (size_t)end;
    }
    return bytes;
}

static bool
files_equal(const char *a_path, const char *b_path, bool *equal_out,
            char **err_out)
{
    *equal_out = false;

    size_t a_len = 0;
    size_t b_len = 0;
    char *a = read_file_bytes(a_path, &a_len, err_out);
    if (!a) {
        return false;
    }
    char *b = read_file_bytes(b_path, &b_len, err_out);
    if (!b) {
        ncc_free(a);
        return false;
    }

    *equal_out = a_len == b_len && memcmp(a, b, a_len) == 0;
    ncc_free(a);
    ncc_free(b);
    return true;
}

static bool
bytes_contain_text(const char *bytes, size_t len, const char *needle)
{
    if (!bytes || !needle) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > len) {
        return false;
    }

    for (size_t i = 0; i + needle_len <= len; i++) {
        if (memcmp(bytes + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool
file_contains_text(const char *path, const char *needle)
{
    size_t len = 0;
    char *bytes = read_file_bytes(path, &len, nullptr);
    if (!bytes) {
        return false;
    }

    bool found = bytes_contain_text(bytes, len, needle);
    ncc_free(bytes);
    return found;
}

static bool
plan_inputs_contain_text(const ncc_comptime_plan_t *plan, const char *needle)
{
    if (!plan || !needle) {
        return false;
    }

    for (int i = 0; i < plan->n_ordered_link_args; i++) {
        if (file_contains_text(plan->ordered_link_args[i], needle)) {
            return true;
        }
    }
    for (int i = 0; i < plan->n_user_inputs; i++) {
        if (file_contains_text(plan->user_inputs[i], needle)) {
            return true;
        }
    }
    for (int i = 0; i < plan->n_runtime_inputs; i++) {
        if (file_contains_text(plan->runtime_inputs[i], needle)) {
            return true;
        }
    }
    return false;
}

static bool
plan_has_object_local_static_init_entries(const ncc_comptime_plan_t *plan)
{
    if (!plan || !plan->meta || plan->meta->n_static_inits <= 0) {
        return true;
    }

    for (int i = 0; i < plan->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &plan->meta->static_inits[i];
        ncc_buffer_t *buf = ncc_buffer_empty();
        ncc_buffer_printf(buf, "__ncc_static_init_degrade_entry_%.*s",
                          (int)si->name.u8_bytes, si->name.data);
        char *sym = ncc_buffer_take(buf);
        bool found = plan_inputs_contain_text(plan, sym);
        ncc_free(sym);
        if (!found) {
            return false;
        }
    }
    return true;
}

static ncc_ct_target_os_t
default_target_os(void)
{
#ifdef _WIN32
    return NCC_CRT_TARGET_WINDOWS;
#elif defined(__APPLE__)
    return NCC_CRT_TARGET_DARWIN;
#else
    return NCC_CRT_TARGET_LINUX;
#endif
}

static ncc_ct_target_os_t
target_os_from_triple(const char *triple, ncc_ct_target_os_t fallback)
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

static ncc_ct_target_os_t
target_os(const ncc_opts_t *opts)
{
    ncc_ct_target_os_t os = default_target_os();

    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (strncmp(arg, "--target=", 9) == 0) {
            os = target_os_from_triple(arg + 9, os);
            continue;
        }
        if (strncmp(arg, "-target=", 8) == 0) {
            os = target_os_from_triple(arg + 8, os);
            continue;
        }
        if ((strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0)
            && i + 1 < opts->n_clang_args) {
            os = target_os_from_triple(opts->clang_args[++i], os);
        }
    }

    return os;
}

static bool
plan_inputs_contain_start_symbol(const ncc_comptime_plan_t *plan,
                                 const ncc_opts_t *opts)
{
    switch (target_os(opts)) {
    case NCC_CRT_TARGET_DARWIN:
        return plan_inputs_contain_text(plan, "_n00b_start");
    case NCC_CRT_TARGET_LINUX:
        return plan_inputs_contain_text(plan, "n00b_start");
    case NCC_CRT_TARGET_WINDOWS:
    default:
        return true;
    }
}

static const char *
platform_entry_flag(const ncc_opts_t *opts)
{
    switch (target_os(opts)) {
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
platform_entry_force_undefined_flag(const ncc_opts_t *opts)
{
    switch (target_os(opts)) {
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

const char **
ncc_collect_comptime_argv(const char *program, const char *const *args,
                          int n_args)
{
    if (!program || n_args < 0) {
        return nullptr;
    }

    const char **argv = ncc_alloc_array(const char *, (size_t)n_args + 2);
    argv[0] = program;
    for (int i = 0; i < n_args; i++) {
        argv[i + 1] = args[i];
    }
    argv[n_args + 1] = nullptr;
    return argv;
}

static bool
compile_entry_object(const ncc_opts_t *opts, ncc_crt_variant_t variant,
                     const ncc_crt_entry_context_t *entry_ctx,
                     ncc_temp_workspace_t *tmp, const char *stem,
                     char **object_path_out, char **err_out)
{
    *object_path_out = nullptr;

    const char *entry_source = ncc_crt_emit_entry_ex(opts, variant, entry_ctx);
    if (!entry_source) {
        set_err(err_out, "failed to generate comptime CRT entry source");
        return false;
    }

    ncc_buffer_t *src_leaf = ncc_buffer_empty();
    ncc_buffer_printf(src_leaf, "%s.c", stem);
    char *src_leaf_text = ncc_buffer_take(src_leaf);
    char *source_path = ncc_temp_workspace_join(tmp, src_leaf_text);
    ncc_free(src_leaf_text);

    ncc_buffer_t *obj_leaf = ncc_buffer_empty();
    ncc_buffer_printf(obj_leaf, "%s.o", stem);
    char *obj_leaf_text = ncc_buffer_take(obj_leaf);
    char *object_path = ncc_temp_workspace_join(tmp, obj_leaf_text);
    ncc_free(obj_leaf_text);

    if (!source_path || !object_path) {
        set_err(err_out, "failed to create comptime CRT temp paths");
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    char *write_err = nullptr;
    if (!ncc_platform_write_file(source_path, entry_source,
                                 strlen(entry_source), &write_err)) {
        set_err(err_out, "write to comptime CRT temp file failed: %s",
                write_err ? write_err : "(no detail)");
        ncc_free(write_err);
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    int max_args = 8 + (opts ? opts->n_clang_args : 0);
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args + 1);
    int ai = 0;

    argv[ai++] = opts->compiler;
    ai = append_compile_target_args(opts, argv, ai);
    argv[ai++] = "-std=gnu23";
    argv[ai++] = "-c";
    argv[ai++] = source_path;
    argv[ai++] = "-o";
    argv[ai++] = object_path;
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
        set_err(err_out, "comptime CRT entry compile failed: %s", stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_free(source_path);

    if (!ok) {
        ncc_free(object_path);
        return false;
    }

    *object_path_out = object_path;
    return true;
}

static const char linux_start_source[] =
    "#if defined(__aarch64__)\n"
    "    .text\n"
    "    .globl n00b_start\n"
    "    .type n00b_start,%function\n"
    "n00b_start:\n"
    "    mov x29, xzr\n"
    "    mov x30, xzr\n"
    "    ldr x0, [sp]\n"
    "    add x1, sp, #8\n"
    "    add x2, x1, x0, lsl #3\n"
    "    add x2, x2, #8\n"
    "    bl n00b_crt_main\n"
    "    brk #0\n"
    "    .size n00b_start, .-n00b_start\n"
    "    .section .note.GNU-stack,\"\",@progbits\n"
    "#elif defined(__x86_64__)\n"
    "    .text\n"
    "    .globl n00b_start\n"
    "    .type n00b_start,@function\n"
    "n00b_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq (%rsp), %rdi\n"
    "    leaq 8(%rsp), %rsi\n"
    "    leaq 16(%rsp,%rdi,8), %rdx\n"
    "    andq $-16, %rsp\n"
    "    call n00b_crt_main\n"
    "    ud2\n"
    "    .size n00b_start, .-n00b_start\n"
    "    .section .note.GNU-stack,\"\",@progbits\n"
    "#else\n"
    "#error unsupported Linux target for ncc custom entry start stub\n"
    "#endif\n";

static bool
compile_start_object(const ncc_opts_t *opts, const ncc_comptime_plan_t *plan,
                     ncc_temp_workspace_t *tmp, char **object_path_out,
                     char **err_out)
{
    *object_path_out = nullptr;

    if (target_os(opts) != NCC_CRT_TARGET_LINUX
        || plan_inputs_contain_start_symbol(plan, opts)) {
        return true;
    }

    char *source_path = ncc_temp_workspace_join(tmp, "comptime-start.S");
    char *object_path = ncc_temp_workspace_join(tmp, "comptime-start.o");
    if (!source_path || !object_path) {
        set_err(err_out, "failed to create comptime CRT start temp paths");
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    char *write_err = nullptr;
    if (!ncc_platform_write_file(source_path, linux_start_source,
                                 strlen(linux_start_source), &write_err)) {
        set_err(err_out, "write to comptime CRT start temp file failed: %s",
                write_err ? write_err : "(no detail)");
        ncc_free(write_err);
        ncc_free(source_path);
        ncc_free(object_path);
        return false;
    }

    int max_args = 6 + (opts ? opts->n_clang_args : 0);
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args + 1);
    int ai = 0;

    argv[ai++] = opts->compiler;
    ai = append_compile_target_args(opts, argv, ai);
    argv[ai++] = "-c";
    argv[ai++] = source_path;
    argv[ai++] = "-o";
    argv[ai++] = object_path;
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
        set_err(err_out, "comptime CRT start compile failed: %s",
                stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(argv);
    ncc_free(source_path);

    if (!ok) {
        ncc_free(object_path);
        return false;
    }

    *object_path_out = object_path;
    return true;
}

static bool
comptime_plan_needs_ro_image_capture(const ncc_comptime_plan_t *plan)
{
    if (plan == nullptr || plan->meta == nullptr) {
        return false;
    }

    for (int i = 0; i < plan->meta->n_vars; i++) {
        if ((plan->meta->vars[i].flags & NCC_CT_VAR_FLAG_POINTER_ROOT) != 0) {
            return true;
        }
    }
    for (int i = 0; i < plan->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &plan->meta->static_inits[i];
        if (si->kind != NCC_CT_STATIC_INIT_WRITABLE) {
            return true;
        }
    }

    return false;
}

static bool
comptime_plan_needs_writable_image_capture(const ncc_comptime_plan_t *plan)
{
    if (plan == nullptr || plan->meta == nullptr) {
        return false;
    }

    for (int i = 0; i < plan->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &plan->meta->static_inits[i];
        if (si->kind == NCC_CT_STATIC_INIT_WRITABLE) {
            return true;
        }
    }

    return false;
}

static bool
comptime_plan_needs_image_capture(const ncc_comptime_plan_t *plan)
{
    return comptime_plan_needs_ro_image_capture(plan)
        || comptime_plan_needs_writable_image_capture(plan);
}

static bool
validate_comptime_image_roots(const ncc_comptime_plan_t *plan, char **err_out)
{
    if (!comptime_plan_needs_image_capture(plan)) {
        return true;
    }

    for (int i = 0; i < plan->meta->n_vars; i++) {
        const ncc_ct_var_t *var = &plan->meta->vars[i];
        if ((var->flags & NCC_CT_VAR_FLAG_POINTER_ROOT) == 0) {
            continue;
        }
        if (var->linkage != 1) {
            set_err(err_out,
                    "comptime variable '%.*s' has internal linkage; Phase 3 "
                    "image baking requires external pointer roots",
                    (int)var->name.u8_bytes, var->name.data);
            return false;
        }
    }
    return true;
}

static bool
link_with_entry(const ncc_opts_t *opts, const ncc_comptime_plan_t *plan,
                const char *entry_object,
                const char *const *extra_objects, int n_extra_objects,
                const char *output_file, char **err_out)
{
    int max_args = 9 + plan->n_ordered_link_args + plan->n_user_inputs
                 + plan->n_runtime_inputs + plan->n_link_args
                 + n_extra_objects;
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args + 1);
    int ai = 0;

    argv[ai++] = opts->compiler;
    argv[ai++] = "-nostartfiles";
    argv[ai++] = platform_entry_flag(opts);
    const char *entry_force = platform_entry_force_undefined_flag(opts);
    if (entry_force) {
        argv[ai++] = entry_force;
    }
    argv[ai++] = "-o";
    argv[ai++] = output_file;
    argv[ai++] = "-x";
    argv[ai++] = "none";
    argv[ai++] = entry_object;

    for (int i = 0; i < plan->n_ordered_link_args; i++) {
        argv[ai++] = plan->ordered_link_args[i];
    }
    if (plan->n_ordered_link_args > 0) {
        for (int i = 0; i < n_extra_objects; i++) {
            argv[ai++] = extra_objects[i];
        }
    }
    for (int i = 0; i < plan->n_runtime_inputs; i++) {
        argv[ai++] = plan->runtime_inputs[i];
    }
    if (plan->n_ordered_link_args == 0) {
        for (int i = 0; i < plan->n_user_inputs; i++) {
            argv[ai++] = plan->user_inputs[i];
        }
        for (int i = 0; i < n_extra_objects; i++) {
            argv[ai++] = extra_objects[i];
        }
        for (int i = 0; i < plan->n_link_args; i++) {
            argv[ai++] = plan->link_args[i];
        }
    }

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
        set_err(err_out, "comptime link failed: %s", stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(argv);
    return ok;
}

static char *
atomic_link_temp_path(const char *output_file)
{
    char *dir = ncc_platform_dirname(output_file);
    if (!dir) {
        return nullptr;
    }

    const char *leaf = output_file;
    for (const char *p = output_file; p && *p; p++) {
        if (*p == '/' || *p == '\\') {
            leaf = p + 1;
        }
    }

    ncc_buffer_t *tmp_leaf = ncc_buffer_empty();
    static unsigned counter = 0;
    unsigned id = ++counter;
#ifdef _WIN32
    ncc_buffer_printf(tmp_leaf, ".%s.ncc-ct-%lu-%u.tmp", leaf,
                      (unsigned long)GetCurrentProcessId(), id);
#else
    ncc_buffer_printf(tmp_leaf, ".%s.ncc-ct-%ld-%u.tmp", leaf,
                      (long)getpid(), id);
#endif
    char *tmp_leaf_text = ncc_buffer_take(tmp_leaf);
    char *path = ncc_platform_join_path(dir, tmp_leaf_text);

    ncc_free(tmp_leaf_text);
    ncc_free(dir);
    return path;
}

static int
run_comptime_executable(const char *program, const char *const *argv,
                        char **err_out)
{
    const char **run_argv = (const char **)argv;

    if (!run_argv) {
        run_argv = ncc_collect_comptime_argv(program, nullptr, 0);
    }

    ncc_process_spec_t spec = {
        .program        = program,
        .argv           = run_argv,
        .capture_stdout = false,
        .capture_stderr = false,
    };
    ncc_process_result_t proc = {0};

    if (!ncc_process_run(&spec, &proc)) {
        char *msg = ncc_comptime_run_failure_message(NCC_CT_RUN_LAUNCH, 0);
        set_err(err_out, "%s: %s", msg,
                proc.stderr_data ? proc.stderr_data : "(no detail)");
        ncc_free(msg);
        ncc_process_result_free(&proc);
        if (!argv) {
            ncc_free((void *)run_argv);
        }
        return 1;
    }

    ncc_ct_run_status_t st = ncc_comptime_run_status(&proc);
    int rc = proc.exit_code;
    if (st == NCC_CT_RUN_SIGNAL) {
        rc = proc.signal_number;
    }
    else if (st == NCC_CT_RUN_EXCEPTION) {
        rc = (int)proc.exception_code;
    }
    ncc_process_result_free(&proc);
    if (!argv) {
        ncc_free((void *)run_argv);
    }

    if (st != NCC_CT_RUN_OK) {
        char *msg = ncc_comptime_run_failure_message(st, rc);
        set_err(err_out, "%s", msg);
        ncc_free(msg);
        return rc;
    }

    return 0;
}

int
ncc_comptime_run_and_link(const ncc_opts_t *opts,
                          const ncc_comptime_plan_t *plan,
                          char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    if (!opts || !opts->compiler || !plan || !plan->output_file
        || !plan->ordered_link_args || plan->n_ordered_link_args <= 0
        || plan->n_comptime_argv < 0
        || (!plan->comptime_argv && plan->n_comptime_argv != 0)) {
        set_err(err_out, "invalid comptime build plan");
        return 1;
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_ct_build_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err
                                       : "failed to create comptime workspace");
        ncc_free(tmp_err);
        return 1;
    }

#ifdef _WIN32
    char *comptime_exe = ncc_temp_workspace_join(&tmp, "comptime.exe");
#else
    char *comptime_exe = ncc_temp_workspace_join(&tmp, "comptime");
#endif
    char *start_entry_obj = nullptr;
    char *run_entry_obj = nullptr;
    char *final_entry_obj = nullptr;
    char *owned_capture_path = nullptr;
    char *owned_writable_capture_path = nullptr;
    char **static_init_snapshot_paths = nullptr;
    char **static_init_check_paths = nullptr;
    char *image_obj = nullptr;
    char *writable_image_obj = nullptr;
    char *atomic_output = nullptr;
    bool atomic_output_published = false;
    int rc = 1;

    if (!comptime_exe) {
        set_err(err_out, "failed to create comptime executable path");
        goto cleanup;
    }

    if (!validate_comptime_image_roots(plan, err_out)) {
        goto cleanup;
    }

    const char *capture_path = plan->captured_image_path;
    if (!capture_path && comptime_plan_needs_ro_image_capture(plan)) {
        owned_capture_path = ncc_temp_workspace_join(&tmp, "comptime-image.bin");
        if (!owned_capture_path) {
            set_err(err_out, "failed to create comptime image capture path");
            goto cleanup;
        }
        capture_path = owned_capture_path;
    }

    const char *writable_capture_path = plan->captured_writable_image_path;
    if (!writable_capture_path
        && comptime_plan_needs_writable_image_capture(plan)) {
        owned_writable_capture_path =
            ncc_temp_workspace_join(&tmp, "comptime-writable-image.bin");
        if (!owned_writable_capture_path) {
            set_err(err_out,
                    "failed to create writable comptime image capture path");
            goto cleanup;
        }
        writable_capture_path = owned_writable_capture_path;
    }

    int n_static_inits = plan->meta ? plan->meta->n_static_inits : 0;
    if (n_static_inits > 0) {
        static_init_snapshot_paths = ncc_alloc_array(char *,
                                                     (size_t)n_static_inits);
        static_init_check_paths = ncc_alloc_array(char *,
                                                  (size_t)n_static_inits);
        for (int i = 0; i < n_static_inits; i++) {
            const ncc_ct_static_init_t *si = &plan->meta->static_inits[i];
            ncc_buffer_t *snap_leaf = ncc_buffer_empty();
            ncc_buffer_t *check_leaf = ncc_buffer_empty();
            ncc_buffer_printf(snap_leaf, "static-init-%.*s-before.bin",
                              (int)si->name.u8_bytes, si->name.data);
            ncc_buffer_printf(check_leaf, "static-init-%.*s-after.bin",
                              (int)si->name.u8_bytes, si->name.data);
            char *snap_leaf_text = ncc_buffer_take(snap_leaf);
            char *check_leaf_text = ncc_buffer_take(check_leaf);
            static_init_snapshot_paths[i] =
                ncc_temp_workspace_join(&tmp, snap_leaf_text);
            static_init_check_paths[i] =
                ncc_temp_workspace_join(&tmp, check_leaf_text);
            ncc_free(snap_leaf_text);
            ncc_free(check_leaf_text);
            if (!static_init_snapshot_paths[i]
                || !static_init_check_paths[i]) {
                set_err(err_out,
                        "failed to create static-init comparison paths");
                goto cleanup;
            }
        }
    }

    ncc_crt_entry_context_t run_entry_ctx = {
        .meta = plan->meta,
        .captured_image_path = capture_path,
        .captured_writable_image_path = writable_capture_path,
        .static_init_snapshot_paths =
            (const char *const *)static_init_snapshot_paths,
        .static_init_check_paths =
            (const char *const *)static_init_check_paths,
    };
    ncc_crt_entry_context_t final_entry_ctx = {
        .meta = plan->meta,
    };

    if (!compile_entry_object(opts, NCC_CRT_VARIANT_COMPTIME_RUN, &run_entry_ctx, &tmp,
                              "comptime-entry", &run_entry_obj, err_out)) {
        goto cleanup;
    }

    if (!compile_start_object(opts, plan, &tmp, &start_entry_obj, err_out)) {
        goto cleanup;
    }

    const char *run_extra_objects[1] = {0};
    int n_run_extra_objects = 0;
    if (start_entry_obj) {
        run_extra_objects[n_run_extra_objects++] = start_entry_obj;
    }
    if (!link_with_entry(opts, plan, run_entry_obj, run_extra_objects,
                         n_run_extra_objects,
                         comptime_exe, err_out)) {
        goto cleanup;
    }

    const char *const *run_args = nullptr;
    int run_arg_count = 0;
    if (plan->comptime_argv && plan->n_comptime_argv > 0) {
        run_args = plan->comptime_argv + 1;
        run_arg_count = plan->n_comptime_argv - 1;
    }
    const char **run_argv = ncc_collect_comptime_argv(comptime_exe, run_args,
                                                      run_arg_count);
    if (!run_argv) {
        set_err(err_out, "failed to create comptime argv");
        rc = 1;
        goto cleanup;
    }

    rc = run_comptime_executable(comptime_exe, run_argv, err_out);
    ncc_free((void *)run_argv);
    if (rc != 0) {
        goto cleanup;
    }

    for (int i = 0; i < n_static_inits; i++) {
        bool equal = false;
        if (!files_equal(static_init_snapshot_paths[i],
                         static_init_check_paths[i], &equal, err_out)) {
            rc = 1;
            goto cleanup;
        }
        if (!equal) {
            const ncc_ct_static_init_t *si = &plan->meta->static_inits[i];
            set_err(err_out,
                    "D-024: static initializer '%.*s' mutated during "
                    "comptime run",
                    (int)si->name.u8_bytes, si->name.data);
            rc = 1;
            goto cleanup;
        }
    }

    if (capture_path) {
        image_obj = ncc_temp_workspace_join(&tmp, "comptime-image.o");
        if (!image_obj) {
            set_err(err_out, "failed to create comptime image object path");
            rc = 1;
            goto cleanup;
        }
        if (!ncc_emit_image_object(opts, capture_path, image_obj,
                                   err_out)) {
            rc = 1;
            goto cleanup;
        }
    }

    if (writable_capture_path) {
        writable_image_obj =
            ncc_temp_workspace_join(&tmp, "comptime-writable-image.o");
        if (!writable_image_obj) {
            set_err(err_out,
                    "failed to create writable comptime image object path");
            rc = 1;
            goto cleanup;
        }
        if (!ncc_emit_writable_image_object(opts, writable_capture_path,
                                            writable_image_obj, err_out)) {
            rc = 1;
            goto cleanup;
        }
    }

    if (!compile_entry_object(opts, NCC_CRT_VARIANT_FINAL, &final_entry_ctx, &tmp,
                              "final-entry", &final_entry_obj, err_out)) {
        rc = 1;
        goto cleanup;
    }

    atomic_output = atomic_link_temp_path(plan->output_file);
    if (!atomic_output) {
        set_err(err_out, "failed to create atomic comptime output path");
        rc = 1;
        goto cleanup;
    }
    (void)ncc_platform_remove_file(atomic_output);

    const char *extra_objects[3] = {0};
    int n_extra_objects = 0;
    if (start_entry_obj) {
        extra_objects[n_extra_objects++] = start_entry_obj;
    }
    if (image_obj) {
        extra_objects[n_extra_objects++] = image_obj;
    }
    if (writable_image_obj) {
        extra_objects[n_extra_objects++] = writable_image_obj;
    }
    if (!link_with_entry(opts, plan, final_entry_obj, extra_objects,
                         n_extra_objects, atomic_output, err_out)) {
        rc = 1;
        goto cleanup;
    }

    if (!ncc_ct_strip_section(opts, atomic_output, err_out)) {
        rc = 1;
        goto cleanup;
    }

    if (!ncc_comptime_emit_output_atomic(plan->output_file, atomic_output,
                                         err_out)) {
        rc = 1;
        goto cleanup;
    }
    atomic_output_published = true;

    rc = 0;

cleanup:
    if (atomic_output && !atomic_output_published) {
        (void)ncc_platform_remove_file(atomic_output);
    }
    ncc_free(comptime_exe);
    ncc_free(start_entry_obj);
    ncc_free(run_entry_obj);
    ncc_free(final_entry_obj);
    ncc_free(owned_capture_path);
    ncc_free(owned_writable_capture_path);
    for (int i = 0; i < (plan && plan->meta ? plan->meta->n_static_inits : 0);
         i++) {
        if (static_init_snapshot_paths) {
            ncc_free(static_init_snapshot_paths[i]);
        }
        if (static_init_check_paths) {
            ncc_free(static_init_check_paths[i]);
        }
    }
    ncc_free(static_init_snapshot_paths);
    ncc_free(static_init_check_paths);
    ncc_free(image_obj);
    ncc_free(writable_image_obj);
    ncc_free(atomic_output);
    ncc_temp_workspace_cleanup(&tmp);
    return rc;
}

int
ncc_comptime_degrade_and_link(const ncc_opts_t *opts,
                              const ncc_comptime_plan_t *plan,
                              char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    if (!opts || !opts->compiler || !plan || !plan->output_file
        || !plan->meta
        || (!plan->meta->has_comptime_main
            && plan->meta->n_static_inits <= 0)
        || !plan->ordered_link_args || plan->n_ordered_link_args <= 0) {
        set_err(err_out, "invalid comptime degrade build plan");
        return 1;
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_ct_degrade_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err
                                       : "failed to create comptime workspace");
        ncc_free(tmp_err);
        return 1;
    }

    char *start_entry_obj = nullptr;
    char *degrade_entry_obj = nullptr;
    char *static_init_degrade_obj = nullptr;
    int rc = 1;

    ncc_crt_entry_context_t degrade_entry_ctx = {
        .meta = plan->meta,
    };
    char *atomic_output = nullptr;
    bool atomic_output_published = false;

    ncc_crt_variant_t entry_variant = plan->meta->has_comptime_main
                                    ? NCC_CRT_VARIANT_DEGRADE
                                    : NCC_CRT_VARIANT_BASE;
    if (!compile_entry_object(opts, entry_variant, &degrade_entry_ctx,
                              &tmp, "degrade-entry", &degrade_entry_obj,
                              err_out)) {
        goto cleanup;
    }
    if (!compile_start_object(opts, plan, &tmp, &start_entry_obj, err_out)) {
        goto cleanup;
    }

    if (plan->meta->n_static_inits > 0
        && !plan_has_object_local_static_init_entries(plan)) {
        static_init_degrade_obj =
            ncc_temp_workspace_join(&tmp, "static-init-degrade.o");
        if (!static_init_degrade_obj) {
            set_err(err_out, "failed to create static-init degrade object path");
            goto cleanup;
        }
        if (!ncc_emit_static_init_degrade_object(opts, plan->meta,
                                                 static_init_degrade_obj,
                                                 err_out)) {
            goto cleanup;
        }
    }

    atomic_output = atomic_link_temp_path(plan->output_file);
    if (!atomic_output) {
        set_err(err_out, "failed to create atomic comptime output path");
        goto cleanup;
    }
    (void)ncc_platform_remove_file(atomic_output);

    const char *extra_objects[2] = {0};
    int n_extra_objects = 0;
    if (start_entry_obj) {
        extra_objects[n_extra_objects++] = start_entry_obj;
    }
    if (static_init_degrade_obj) {
        extra_objects[n_extra_objects++] = static_init_degrade_obj;
    }
    if (!link_with_entry(opts, plan, degrade_entry_obj, extra_objects,
                         n_extra_objects, atomic_output, err_out)) {
        goto cleanup;
    }

    if (!ncc_ct_strip_section(opts, atomic_output, err_out)) {
        goto cleanup;
    }

    if (!ncc_comptime_emit_output_atomic(plan->output_file, atomic_output,
                                         err_out)) {
        goto cleanup;
    }
    atomic_output_published = true;

    rc = 0;

cleanup:
    if (atomic_output && !atomic_output_published) {
        (void)ncc_platform_remove_file(atomic_output);
    }
    ncc_free(start_entry_obj);
    ncc_free(degrade_entry_obj);
    ncc_free(static_init_degrade_obj);
    ncc_free(atomic_output);
    ncc_temp_workspace_cleanup(&tmp);
    return rc;
}
