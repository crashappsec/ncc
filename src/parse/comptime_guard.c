#include "parse/comptime_guard.h"

#include "lib/alloc.h"
#include "lib/buffer.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#endif

typedef enum {
    NCC_CT_OS_UNKNOWN,
    NCC_CT_OS_LINUX,
    NCC_CT_OS_DARWIN,
    NCC_CT_OS_WINDOWS,
} ncc_ct_os_t;

typedef enum {
    NCC_CT_ARCH_UNKNOWN,
    NCC_CT_ARCH_X86_64,
    NCC_CT_ARCH_AARCH64,
} ncc_ct_arch_t;

typedef struct {
    ncc_ct_os_t   os;
    ncc_ct_arch_t arch;
    bool          specified;
} ncc_ct_target_t;

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

static const char *
os_name(ncc_ct_os_t os)
{
    switch (os) {
    case NCC_CT_OS_LINUX:
        return "linux";
    case NCC_CT_OS_DARWIN:
        return "darwin";
    case NCC_CT_OS_WINDOWS:
        return "windows";
    case NCC_CT_OS_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *
arch_name(ncc_ct_arch_t arch)
{
    switch (arch) {
    case NCC_CT_ARCH_X86_64:
        return "x86_64";
    case NCC_CT_ARCH_AARCH64:
        return "aarch64";
    case NCC_CT_ARCH_UNKNOWN:
    default:
        return "unknown";
    }
}

static ncc_ct_os_t
host_os(void)
{
#ifdef _WIN32
    return NCC_CT_OS_WINDOWS;
#elif defined(__APPLE__)
    return NCC_CT_OS_DARWIN;
#elif defined(__linux__)
    return NCC_CT_OS_LINUX;
#else
    return NCC_CT_OS_UNKNOWN;
#endif
}

static ncc_ct_arch_t
host_arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return NCC_CT_ARCH_X86_64;
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return NCC_CT_ARCH_AARCH64;
#else
    return NCC_CT_ARCH_UNKNOWN;
#endif
}

const char *
ncc_host_triple(void)
{
    static char buf[64];

    snprintf(buf, sizeof(buf), "%s-unknown-%s",
             arch_name(host_arch()), os_name(host_os()));
    return buf;
}

static ncc_ct_arch_t
arch_from_text(const char *text)
{
    if (!text) {
        return NCC_CT_ARCH_UNKNOWN;
    }
    if (strstr(text, "x86_64") || strstr(text, "amd64")) {
        return NCC_CT_ARCH_X86_64;
    }
    if (strstr(text, "aarch64") || strstr(text, "arm64")) {
        return NCC_CT_ARCH_AARCH64;
    }
    return NCC_CT_ARCH_UNKNOWN;
}

static ncc_ct_os_t
os_from_text(const char *text)
{
    if (!text) {
        return NCC_CT_OS_UNKNOWN;
    }
    if (strstr(text, "linux") || strstr(text, "android")) {
        return NCC_CT_OS_LINUX;
    }
    if (strstr(text, "darwin") || strstr(text, "apple")
        || strstr(text, "macos") || strstr(text, "ios")
        || strstr(text, "tvos") || strstr(text, "watchos")) {
        return NCC_CT_OS_DARWIN;
    }
    if (strstr(text, "windows") || strstr(text, "mingw")
        || strstr(text, "msvc") || strstr(text, "cygwin")) {
        return NCC_CT_OS_WINDOWS;
    }
    return NCC_CT_OS_UNKNOWN;
}

static bool
looks_like_standalone_triple(const char *arg)
{
    if (!arg || arg[0] == '-' || strchr(arg, '/') || strchr(arg, '.')
        || !strchr(arg, '-')) {
        return false;
    }

    ncc_ct_arch_t arch = arch_from_text(arg);
    ncc_ct_os_t   os   = os_from_text(arg);
    return arch != NCC_CT_ARCH_UNKNOWN && os != NCC_CT_OS_UNKNOWN;
}

static void
target_from_triple(ncc_ct_target_t *target, const char *triple)
{
    target->specified = true;
    target->arch = arch_from_text(triple);
    target->os = os_from_text(triple);
}

static void
target_from_arch(ncc_ct_target_t *target, const char *arch)
{
    target->specified = true;
    target->arch = arch_from_text(arch);
    if (target->os == NCC_CT_OS_UNKNOWN) {
        target->os = host_os();
    }
}

static ncc_ct_target_t
detect_target(const ncc_opts_t *opts)
{
    ncc_ct_target_t target = {
        .os = NCC_CT_OS_UNKNOWN,
        .arch = NCC_CT_ARCH_UNKNOWN,
        .specified = false,
    };

    for (int i = 0; opts && i < opts->n_clang_args; i++) {
        const char *arg = opts->clang_args[i];

        if (strncmp(arg, "--target=", 9) == 0) {
            target_from_triple(&target, arg + 9);
            continue;
        }
        if (strncmp(arg, "-target=", 8) == 0) {
            target_from_triple(&target, arg + 8);
            continue;
        }
        if ((strcmp(arg, "-target") == 0 || strcmp(arg, "--target") == 0)
            && i + 1 < opts->n_clang_args) {
            target_from_triple(&target, opts->clang_args[++i]);
            continue;
        }
        if (strcmp(arg, "-arch") == 0 && i + 1 < opts->n_clang_args) {
            target_from_arch(&target, opts->clang_args[++i]);
            continue;
        }
        if (looks_like_standalone_triple(arg)) {
            target_from_triple(&target, arg);
        }
    }

    return target;
}

bool
ncc_target_is_host(const ncc_opts_t *opts)
{
    ncc_ct_target_t target = detect_target(opts);
    if (!target.specified) {
        return true;
    }

    return target.os == host_os() && target.arch == host_arch();
}

bool
ncc_comptime_guard_check(const ncc_opts_t *opts,
                         const ncc_ct_aggregate_t *agg, char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }
    if (!agg || !agg->has_comptime_main) {
        return true;
    }
    if (opts && opts->no_comptime) {
        return true;
    }
    ncc_ct_target_t target = detect_target(opts);
    if (!target.specified
        || (target.os == host_os() && target.arch == host_arch())) {
        return true;
    }
    if ((agg->main_flags & NCC_CT_MAIN_FLAG_OPTIONAL) != 0) {
        return true;
    }

    ncc_buffer_t *target_buf = ncc_buffer_empty();
    ncc_buffer_printf(target_buf, "%s-unknown-%s",
                      arch_name(target.arch), os_name(target.os));
    char *target_text = ncc_buffer_take(target_buf);

    set_err(err_out,
            "cannot run comptime due to platform mismatch: host=%s target=%s",
            ncc_host_triple(), target_text);
    ncc_free(target_text);
    return false;
}

ncc_comptime_degrade_route_t
ncc_comptime_degrade_route(const ncc_opts_t *opts,
                           const ncc_ct_aggregate_t *agg)
{
    bool target_is_host = ncc_target_is_host(opts);
    bool no_comptime = opts != nullptr && opts->no_comptime;
    bool optional_target_mismatch =
        agg != nullptr && agg->has_comptime_main && !target_is_host
        && (agg->main_flags & NCC_CT_MAIN_FLAG_OPTIONAL) != 0;

    return (ncc_comptime_degrade_route_t){
        .target_is_host = target_is_host,
        .comptime_main_degrade =
            agg != nullptr && agg->has_comptime_main
            && (no_comptime || optional_target_mismatch),
        .static_init_degrade =
            agg != nullptr && agg->n_static_inits > 0
            && (no_comptime || !target_is_host),
    };
}

bool
ncc_static_init_degrade_allowed(const ncc_ct_aggregate_t *agg, char **err_out)
{
    if (err_out != nullptr) {
        *err_out = nullptr;
    }

    for (int i = 0; agg != nullptr && i < agg->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &agg->static_inits[i];
        if (si->degrade_ok != 0) {
            continue;
        }

        set_err(err_out,
                "static initializer '%.*s' cannot be lowered to runtime "
                "initialization for this target",
                (int)si->name.u8_bytes, si->name.data);
        return false;
    }

    return true;
}

ncc_ct_run_status_t
ncc_comptime_run_status(const ncc_process_result_t *proc)
{
    if (!proc) {
        return NCC_CT_RUN_LAUNCH;
    }

    switch (proc->term_kind) {
    case NCC_PROCESS_TERM_EXITED:
        return proc->exit_code == 0 ? NCC_CT_RUN_OK : NCC_CT_RUN_EXIT;
    case NCC_PROCESS_TERM_SIGNALED:
        return NCC_CT_RUN_SIGNAL;
    case NCC_PROCESS_TERM_EXCEPTION:
        return NCC_CT_RUN_EXCEPTION;
    case NCC_PROCESS_TERM_LAUNCH:
    case NCC_PROCESS_TERM_UNKNOWN:
    default:
        return NCC_CT_RUN_LAUNCH;
    }
}

char *
ncc_comptime_run_failure_message(ncc_ct_run_status_t st, int code)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    switch (st) {
    case NCC_CT_RUN_EXIT:
        ncc_buffer_printf(buf, "comptime exited with status %d", code);
        break;
    case NCC_CT_RUN_SIGNAL:
        ncc_buffer_printf(buf, "comptime execution crashed: signal %d", code);
        break;
    case NCC_CT_RUN_EXCEPTION:
        ncc_buffer_printf(buf,
                          "comptime execution crashed: exception 0x%08x",
                          (unsigned)code);
        break;
    case NCC_CT_RUN_LAUNCH:
        ncc_buffer_puts(buf, "comptime helper launch failed");
        break;
    case NCC_CT_RUN_OK:
    default:
        ncc_buffer_puts(buf, "comptime execution failed");
        break;
    }

    return ncc_buffer_take(buf);
}

bool
ncc_comptime_emit_output_atomic(const char *output_file, const char *temp_path,
                                char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }
    if (!output_file || !output_file[0] || !temp_path || !temp_path[0]) {
        set_err(err_out, "invalid atomic comptime output paths");
        return false;
    }

#ifdef _WIN32
    wchar_t *src = nullptr;
    wchar_t *dst = nullptr;
    int src_len = MultiByteToWideChar(CP_UTF8, 0, temp_path, -1, nullptr, 0);
    int dst_len = MultiByteToWideChar(CP_UTF8, 0, output_file, -1, nullptr, 0);
    if (src_len <= 0 || dst_len <= 0) {
        set_err(err_out, "failed to encode atomic comptime output paths");
        return false;
    }
    src = ncc_alloc_array(wchar_t, (size_t)src_len);
    dst = ncc_alloc_array(wchar_t, (size_t)dst_len);
    bool encoded =
        MultiByteToWideChar(CP_UTF8, 0, temp_path, -1, src, src_len) > 0
        && MultiByteToWideChar(CP_UTF8, 0, output_file, -1, dst, dst_len) > 0;
    if (!encoded) {
        set_err(err_out, "failed to encode atomic comptime output paths");
        ncc_free(src);
        ncc_free(dst);
        return false;
    }
    BOOL ok = MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING
                                    | MOVEFILE_WRITE_THROUGH);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    ncc_free(src);
    ncc_free(dst);
    if (!ok) {
        set_err(err_out, "atomic comptime output rename failed: error %lu",
                (unsigned long)err);
        return false;
    }
    return true;
#else
    if (rename(temp_path, output_file) != 0) {
        set_err(err_out, "atomic comptime output rename failed: %s",
                strerror(errno));
        return false;
    }
    return true;
#endif
}
