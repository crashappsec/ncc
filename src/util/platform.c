#include "util/platform.h"

#include "lib/alloc.h"
#include "lib/buffer.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

static bool ncc_platform_remove_tree(const char *path);

static char *
ncc_strdup(const char *s)
{
    size_t len = strlen(s);
    char  *dup = (char *)ncc_alloc_size(1, len + 1);

    memcpy(dup, s, len + 1);
    return dup;
}

static void
set_error_string(char **out, const char *fmt, ...)
{
    if (!out) {
        return;
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;

    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);

    *out = ncc_buffer_take(buf);
}

static bool
platform_verbose_enabled(void)
{
    const char *value = getenv("NCC_VERBOSE");

    return value && value[0] && strcmp(value, "0") != 0;
}

static void
platform_verbose(const char *fmt, ...)
{
    if (!platform_verbose_enabled()) {
        return;
    }

    va_list ap;

    fprintf(stderr, "ncc: process: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void
platform_verbose_argv(const char *prefix, const char **argv)
{
    if (!platform_verbose_enabled() || !prefix || !argv) {
        return;
    }

    fprintf(stderr, "ncc: process: %s:", prefix);
    for (int i = 0; argv[i]; i++) {
        fprintf(stderr, " %s", argv[i]);
    }
    fputc('\n', stderr);
}

void
ncc_process_result_free(ncc_process_result_t *out)
{
    if (!out) {
        return;
    }

    ncc_free(out->stdout_data);
    ncc_free(out->stderr_data);
    out->stdout_data = nullptr;
    out->stdout_len  = 0;
    out->stderr_data = nullptr;
    out->stderr_len  = 0;
    out->exit_code   = 0;
}

static void
process_result_init(ncc_process_result_t *out)
{
    if (!out) {
        return;
    }

    out->exit_code   = -1;
    out->stdout_data = nullptr;
    out->stdout_len  = 0;
    out->stderr_data = nullptr;
    out->stderr_len  = 0;
}

static bool
path_is_sep(char c)
{
    return c == '/' || c == '\\';
}

char *
ncc_platform_dirname(const char *path)
{
    if (!path || !path[0]) {
        return ncc_strdup(".");
    }

    const char *last = nullptr;

    for (const char *p = path; *p; p++) {
        if (path_is_sep(*p)) {
            last = p;
        }
    }

    if (!last) {
        return ncc_strdup(".");
    }

    if (last == path) {
        char *root = (char *)ncc_alloc_size(1, 2);
        root[0] = path[0];
        root[1] = '\0';
        return root;
    }

    if (last == path + 2 && path[1] == ':') {
        char *drive_root = (char *)ncc_alloc_size(1, 4);
        drive_root[0] = path[0];
        drive_root[1] = path[1];
        drive_root[2] = path[2];
        drive_root[3] = '\0';
        return drive_root;
    }

    size_t len = (size_t)(last - path);

    while (len > 1 && path_is_sep(path[len - 1])) {
        len--;
    }

    char *dir = (char *)ncc_alloc_size(1, len + 1);
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

char *
ncc_platform_join_path(const char *dir, const char *leaf)
{
    if (!dir || !dir[0]) {
        return ncc_strdup(leaf ? leaf : "");
    }

    if (!leaf || !leaf[0]) {
        return ncc_strdup(dir);
    }

#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    bool needs_sep = !path_is_sep(dir[strlen(dir) - 1]) && !path_is_sep(leaf[0]);

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_puts(buf, dir);
    if (needs_sep) {
        ncc_buffer_putc(buf, sep);
    }
    ncc_buffer_puts(buf, leaf);
    return ncc_buffer_take(buf);
}

#ifdef _WIN32

static wchar_t *
utf8_to_utf16(const char *text)
{
    if (!text) {
        return nullptr;
    }

    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);

    if (needed <= 0) {
        return nullptr;
    }

    wchar_t *wide = (wchar_t *)ncc_alloc_size(sizeof(wchar_t), (size_t)needed);

    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed)) {
        ncc_free(wide);
        return nullptr;
    }

    return wide;
}

static char *
utf16_to_utf8(const wchar_t *text)
{
    if (!text) {
        return nullptr;
    }

    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                     nullptr, nullptr);

    if (needed <= 0) {
        return nullptr;
    }

    char *utf8 = (char *)ncc_alloc_size(1, (size_t)needed);

    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, needed,
                             nullptr, nullptr)) {
        ncc_free(utf8);
        return nullptr;
    }

    return utf8;
}

static void
set_windows_error_string(char **out, const char *prefix, DWORD err)
{
    if (!out) {
        return;
    }

    LPWSTR wide_msg = nullptr;
    DWORD  flags    = FORMAT_MESSAGE_ALLOCATE_BUFFER
                   | FORMAT_MESSAGE_FROM_SYSTEM
                   | FORMAT_MESSAGE_IGNORE_INSERTS;

    DWORD len = FormatMessageW(flags, nullptr, err,
                               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               (LPWSTR)&wide_msg, 0, nullptr);

    char *utf8_msg = nullptr;
    if (len > 0 && wide_msg) {
        utf8_msg = utf16_to_utf8(wide_msg);
        LocalFree(wide_msg);
    }

    if (utf8_msg) {
        size_t msg_len = strlen(utf8_msg);
        while (msg_len > 0 && isspace((unsigned char)utf8_msg[msg_len - 1])) {
            utf8_msg[--msg_len] = '\0';
        }
        set_error_string(out, "%s: %s", prefix, utf8_msg);
        ncc_free(utf8_msg);
    } else {
        set_error_string(out, "%s: Windows error %lu", prefix,
                         (unsigned long)err);
    }
}

static char *
windows_normalize_path_utf8(char *path)
{
    if (!path) {
        return nullptr;
    }

    if (strncmp(path, "\\\\?\\UNC\\", 8) == 0) {
        memmove(path + 2, path + 7, strlen(path + 7) + 1);
        path[0] = '\\';
        path[1] = '\\';
    } else if (strncmp(path, "\\\\?\\", 4) == 0) {
        memmove(path, path + 4, strlen(path + 4) + 1);
    }

    return path;
}

char *
ncc_platform_realpath(const char *path)
{
    if (!path || !path[0]) {
        return nullptr;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        return nullptr;
    }

    HANDLE handle = CreateFileW(wpath, 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE
                                | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ncc_free(wpath);

    if (handle != INVALID_HANDLE_VALUE) {
        DWORD len = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                              FILE_NAME_NORMALIZED);
        if (len > 0) {
            wchar_t *wide = (wchar_t *)ncc_alloc_size(sizeof(wchar_t),
                                                      (size_t)len + 1);
            if (GetFinalPathNameByHandleW(handle, wide, len + 1,
                                          FILE_NAME_NORMALIZED) > 0) {
                CloseHandle(handle);
                char *utf8 = utf16_to_utf8(wide);
                ncc_free(wide);
                return windows_normalize_path_utf8(utf8);
            }
            ncc_free(wide);
        }
        CloseHandle(handle);
    }

    wpath = utf8_to_utf16(path);
    if (!wpath) {
        return nullptr;
    }

    DWORD len = GetFullPathNameW(wpath, 0, nullptr, nullptr);
    if (len == 0) {
        ncc_free(wpath);
        return nullptr;
    }

    wchar_t *wide = (wchar_t *)ncc_alloc_size(sizeof(wchar_t), (size_t)len + 1);
    if (!GetFullPathNameW(wpath, len + 1, wide, nullptr)) {
        ncc_free(wpath);
        ncc_free(wide);
        return nullptr;
    }

    ncc_free(wpath);
    char *utf8 = utf16_to_utf8(wide);
    ncc_free(wide);
    return windows_normalize_path_utf8(utf8);
}

char *
ncc_platform_get_exe_path(void)
{
    DWORD len = 0;
    DWORD cap = MAX_PATH;
    wchar_t *wide = nullptr;

    for (;;) {
        ncc_free(wide);
        wide = (wchar_t *)ncc_alloc_size(sizeof(wchar_t), (size_t)cap + 1);
        len  = GetModuleFileNameW(nullptr, wide, cap + 1);

        if (len == 0) {
            ncc_free(wide);
            return nullptr;
        }
        if (len < cap) {
            break;
        }
        cap *= 2;
    }

    char *utf8 = utf16_to_utf8(wide);
    ncc_free(wide);
    return windows_normalize_path_utf8(utf8);
}

bool
ncc_platform_write_file(const char *path, const char *data, size_t len,
                        char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        set_error_string(err_out, "invalid UTF-8 path");
        return false;
    }

    FILE *f = _wfopen(wpath, L"wb");
    ncc_free(wpath);

    if (!f) {
        set_error_string(err_out, "open failed: %s", strerror(errno));
        return false;
    }

    size_t written = fwrite(data, 1, len, f);
    int    close_rc = fclose(f);
    bool   ok       = written == len && close_rc == 0;

    if (!ok) {
        set_error_string(err_out, "write failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static void
normalize_cmp_path(char *path)
{
    for (char *p = path; p && *p; p++) {
        if (*p == '/') {
            *p = '\\';
        }
        *p = (char)tolower((unsigned char)*p);
    }
}

bool
ncc_platform_path_eq(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }

    char *l = ncc_platform_realpath(lhs);
    char *r = ncc_platform_realpath(rhs);

    if (!l || !r) {
        ncc_free(l);
        ncc_free(r);
        return false;
    }

    normalize_cmp_path(l);
    normalize_cmp_path(r);

    bool same = strcmp(l, r) == 0;
    ncc_free(l);
    ncc_free(r);
    return same;
}

char *
ncc_platform_temp_dir_create(const char *prefix)
{
    wchar_t temp_root[MAX_PATH + 1];

    DWORD root_len = GetTempPathW(MAX_PATH, temp_root);
    if (root_len == 0 || root_len > MAX_PATH) {
        return nullptr;
    }

    char prefix_buf[4] = "ncc";
    if (prefix && prefix[0]) {
        size_t len = 0;
        for (const char *p = prefix; *p && len < sizeof(prefix_buf) - 1; p++) {
            if (isalnum((unsigned char)*p)) {
                prefix_buf[len++] = *p;
            }
        }
        prefix_buf[len] = '\0';
        if (len == 0) {
            memcpy(prefix_buf, "ncc", 4);
        }
    }

    wchar_t wprefix[4];
    int prefix_len = MultiByteToWideChar(CP_UTF8, 0, prefix_buf, -1,
                                         wprefix, (int)(sizeof(wprefix) / sizeof(*wprefix)));
    if (prefix_len <= 0) {
        return nullptr;
    }

    wchar_t temp_file[MAX_PATH + 1];
    if (!GetTempFileNameW(temp_root, wprefix, 0, temp_file)) {
        return nullptr;
    }

    DeleteFileW(temp_file);
    if (!CreateDirectoryW(temp_file, nullptr)) {
        return nullptr;
    }

    return utf16_to_utf8(temp_file);
}

bool
ncc_platform_remove_file(const char *path)
{
    if (!path) {
        return true;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        return false;
    }

    int rc = _wremove(wpath);
    ncc_free(wpath);

    return rc == 0 || errno == ENOENT;
}

bool
ncc_platform_remove_dir(const char *path)
{
    if (!path) {
        return true;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        return false;
    }

    int rc = _wrmdir(wpath);
    ncc_free(wpath);

    return rc == 0 || errno == ENOENT;
}

static bool
windows_is_dot_dir(const wchar_t *name)
{
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

static bool
ncc_platform_remove_tree(const char *path)
{
    if (!path || !path[0]) {
        return true;
    }

    wchar_t *wpath = utf8_to_utf16(path);
    if (!wpath) {
        return false;
    }

    DWORD attrs = GetFileAttributesW(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        ncc_free(wpath);
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        bool ok = DeleteFileW(wpath)
               || GetLastError() == ERROR_FILE_NOT_FOUND;
        ncc_free(wpath);
        return ok;
    }

    char *pattern = ncc_platform_join_path(path, "*");
    wchar_t *wpattern = utf8_to_utf16(pattern);
    ncc_free(pattern);

    bool ok = wpattern != nullptr;
    if (ok) {
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileW(wpattern, &data);

        if (find == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            ok = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        } else {
            do {
                if (windows_is_dot_dir(data.cFileName)) {
                    continue;
                }

                char *leaf = utf16_to_utf8(data.cFileName);
                char *child = leaf ? ncc_platform_join_path(path, leaf)
                                   : nullptr;
                ncc_free(leaf);

                if (!child || !ncc_platform_remove_tree(child)) {
                    ok = false;
                }
                ncc_free(child);
            } while (FindNextFileW(find, &data));

            DWORD err = GetLastError();
            if (err != ERROR_NO_MORE_FILES) {
                ok = false;
            }
            FindClose(find);
        }
    }

    ncc_free(wpattern);

    if (!RemoveDirectoryW(wpath)) {
        DWORD err = GetLastError();
        ok = ok && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
    }
    ncc_free(wpath);
    return ok;
}

static void
append_windows_quoted_arg(ncc_buffer_t *buf, const char *arg)
{
    bool needs_quotes = !arg[0];

    for (const char *p = arg; *p && !needs_quotes; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') {
            needs_quotes = true;
        }
    }

    if (!needs_quotes) {
        ncc_buffer_puts(buf, arg);
        return;
    }

    ncc_buffer_putc(buf, '"');

    size_t slash_count = 0;

    for (const char *p = arg; *p; p++) {
        if (*p == '\\') {
            slash_count++;
            continue;
        }

        if (*p == '"') {
            for (size_t i = 0; i < slash_count * 2 + 1; i++) {
                ncc_buffer_putc(buf, '\\');
            }
            ncc_buffer_putc(buf, '"');
            slash_count = 0;
            continue;
        }

        for (size_t i = 0; i < slash_count; i++) {
            ncc_buffer_putc(buf, '\\');
        }
        slash_count = 0;
        ncc_buffer_putc(buf, *p);
    }

    for (size_t i = 0; i < slash_count * 2; i++) {
        ncc_buffer_putc(buf, '\\');
    }

    ncc_buffer_putc(buf, '"');
}

static wchar_t *
build_windows_command_line(const char **argv)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (int i = 0; argv && argv[i]; i++) {
        if (i > 0) {
            ncc_buffer_putc(buf, ' ');
        }
        append_windows_quoted_arg(buf, argv[i]);
    }

    char *utf8 = ncc_buffer_take(buf);
    wchar_t *wide = utf8_to_utf16(utf8);
    ncc_free(utf8);
    return wide;
}

static bool
windows_program_has_path_component(const char *program)
{
    if (!program || !program[0]) {
        return false;
    }

    if (program[1] == ':') {
        return true;
    }

    for (const char *p = program; *p; p++) {
        if (path_is_sep(*p)) {
            return true;
        }
    }

    return false;
}

static bool
windows_program_has_extension(const char *program)
{
    const char *base = program;

    for (const char *p = program; p && *p; p++) {
        if (path_is_sep(*p) || *p == ':') {
            base = p + 1;
        }
    }

    for (const char *p = base; p && *p; p++) {
        if (*p == '.' && p[1] != '\0') {
            return true;
        }
    }

    return false;
}

static char *
windows_path_segment_copy(const char *start, size_t len)
{
    while (len >= 2 && start[0] == '"' && start[len - 1] == '"') {
        start++;
        len -= 2;
    }

    char *copy = (char *)ncc_alloc_size(1, len + 1);
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static bool
windows_path_entry_is_cwd(const char *dir)
{
    if (!dir || !dir[0] || strcmp(dir, ".") == 0) {
        return true;
    }

    char *cwd = ncc_platform_realpath(".");
    bool  same = cwd && ncc_platform_path_eq(dir, cwd);

    ncc_free(cwd);
    return same;
}

static bool
windows_file_exists(const char *path)
{
    wchar_t *wide = utf8_to_utf16(path);
    if (!wide) {
        return false;
    }

    DWORD attrs = GetFileAttributesW(wide);
    ncc_free(wide);

    return attrs != INVALID_FILE_ATTRIBUTES
        && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static char *
windows_join_path_with_extension(const char *dir, const char *program,
                                 const char *ext)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_puts(buf, dir);
    if (dir[0] && !path_is_sep(dir[strlen(dir) - 1])) {
        ncc_buffer_putc(buf, '\\');
    }
    ncc_buffer_puts(buf, program);
    if (ext) {
        ncc_buffer_puts(buf, ext);
    }
    return ncc_buffer_take(buf);
}

static char *
windows_resolve_program_on_path(const char *program)
{
    const char *path = getenv("PATH");

    if (!path || !path[0]) {
        return nullptr;
    }

    static const char *const executable_extensions[] = {
        "",
        ".exe",
        ".com",
        nullptr,
    };
    bool has_extension = windows_program_has_extension(program);

    const char *entry = path;
    for (;;) {
        const char *end = strchr(entry, ';');
        size_t      len = end ? (size_t)(end - entry) : strlen(entry);
        char       *dir = windows_path_segment_copy(entry, len);

        if (!windows_path_entry_is_cwd(dir)) {
            for (int i = 0; executable_extensions[i]; i++) {
                const char *ext = executable_extensions[i];
                if (has_extension && ext[0] != '\0') {
                    continue;
                }

                char *candidate = windows_join_path_with_extension(dir,
                                                                    program,
                                                                    ext);
                if (windows_file_exists(candidate)) {
                    char *resolved = ncc_platform_realpath(candidate);
                    ncc_free(dir);
                    if (resolved) {
                        ncc_free(candidate);
                        return resolved;
                    }
                    return candidate;
                }
                ncc_free(candidate);
            }
        }

        ncc_free(dir);
        if (!end) {
            break;
        }
        entry = end + 1;
    }

    return nullptr;
}

static char *
windows_resolve_process_application(const char *program, char **err_out)
{
    if (windows_program_has_path_component(program)) {
        char *resolved = ncc_platform_realpath(program);
        return resolved ? resolved : ncc_strdup(program);
    }

    char *resolved = windows_resolve_program_on_path(program);
    if (!resolved) {
        set_error_string(err_out, "executable not found on PATH: %s", program);
    }

    return resolved;
}

static bool
create_child_pipe(HANDLE *parent_end, HANDLE *child_end, bool child_reads,
                  char **err_out)
{
    SECURITY_ATTRIBUTES sa = {
        .nLength = sizeof(sa),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = TRUE,
    };

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;

    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        set_windows_error_string(err_out, "CreatePipe failed", GetLastError());
        return false;
    }

    HANDLE parent = child_reads ? write_end : read_end;
    HANDLE child  = child_reads ? read_end : write_end;

    if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        set_windows_error_string(err_out, "SetHandleInformation failed",
                                 GetLastError());
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }

    *parent_end = parent;
    *child_end  = child;
    return true;
}

static bool
write_all_handle(HANDLE handle, const char *data, size_t len, char **err_out)
{
    size_t written = 0;

    while (written < len) {
        DWORD chunk = (DWORD)ncc_min(len - written, (size_t)0x40000000);
        DWORD out   = 0;

        if (!WriteFile(handle, data + written, chunk, &out, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                return true;
            }
            set_windows_error_string(err_out, "WriteFile failed", err);
            return false;
        }

        if (out == 0) {
            set_error_string(err_out, "WriteFile wrote zero bytes");
            return false;
        }

        written += (size_t)out;
    }

    return true;
}

static bool
read_all_handle(HANDLE handle, ncc_buffer_t *buf, char **err_out)
{
    for (;;) {
        char  chunk[4096];
        DWORD got     = 0;

        if (!ReadFile(handle, chunk, sizeof(chunk), &got, nullptr)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                return true;
            }
            set_windows_error_string(err_out, "ReadFile failed", err);
            return false;
        }

        if (got == 0) {
            return true;
        }

        ncc_buffer_append(buf, chunk, (size_t)got);
    }
}

typedef struct {
    HANDLE handle;
    const char *data;
    size_t len;
    ncc_buffer_t *buf;
    bool ok;
    char *err;
} windows_pump_ctx_t;

static DWORD WINAPI
write_pump_thread(LPVOID param)
{
    windows_pump_ctx_t *ctx = (windows_pump_ctx_t *)param;

    ctx->ok = write_all_handle(ctx->handle, ctx->data, ctx->len, &ctx->err);
    CloseHandle(ctx->handle);
    ctx->handle = nullptr;
    return 0;
}

static DWORD WINAPI
read_pump_thread(LPVOID param)
{
    windows_pump_ctx_t *ctx = (windows_pump_ctx_t *)param;

    ctx->ok = read_all_handle(ctx->handle, ctx->buf, &ctx->err);
    CloseHandle(ctx->handle);
    ctx->handle = nullptr;
    return 0;
}

static bool
start_pump_thread(HANDLE *thread_out, windows_pump_ctx_t *ctx,
                  LPTHREAD_START_ROUTINE proc, char **err_out)
{
    *thread_out = CreateThread(nullptr, 0, proc, ctx, 0, nullptr);
    if (!*thread_out) {
        set_windows_error_string(err_out, "CreateThread failed",
                                 GetLastError());
        return false;
    }

    return true;
}

static void
close_thread_if_open(HANDLE *thread)
{
    if (thread && *thread) {
        CloseHandle(*thread);
        *thread = nullptr;
    }
}

static void
close_handle_if_open(HANDLE *handle)
{
    if (handle && *handle) {
        CloseHandle(*handle);
        *handle = nullptr;
    }
}

typedef struct {
    STARTUPINFOEXW info;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes;
    HANDLE duplicated_handles[3];
    HANDLE inherited_handles[3];
    DWORD inherited_count;
} windows_startup_context_t;

static bool
windows_duplicate_startup_handle(HANDLE original, HANDLE *duplicate,
                                 char **err_out)
{
    *duplicate = nullptr;

    if (!original || original == INVALID_HANDLE_VALUE) {
        return true;
    }

    HANDLE current_process = GetCurrentProcess();

    if (!DuplicateHandle(current_process, original, current_process, duplicate,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        set_windows_error_string(err_out, "DuplicateHandle failed",
                                 GetLastError());
        return false;
    }

    return true;
}

static void
windows_add_inherited_handle(windows_startup_context_t *ctx, HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        return;
    }

    for (DWORD i = 0; i < ctx->inherited_count; i++) {
        if (ctx->inherited_handles[i] == handle) {
            return;
        }
    }

    ctx->inherited_handles[ctx->inherited_count++] = handle;
}

static void
windows_startup_context_free(windows_startup_context_t *ctx)
{
    if (!ctx) {
        return;
    }

    if (ctx->attributes) {
        DeleteProcThreadAttributeList(ctx->attributes);
        ncc_free(ctx->attributes);
        ctx->attributes = nullptr;
    }

    for (int i = 0; i < 3; i++) {
        close_handle_if_open(&ctx->duplicated_handles[i]);
    }

    ctx->inherited_count = 0;
}

static bool
windows_startup_context_init(windows_startup_context_t *ctx, HANDLE stdin_handle,
                             HANDLE stdout_handle, HANDLE stderr_handle,
                             char **err_out)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->info.StartupInfo.cb      = sizeof(ctx->info);
    ctx->info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    if (!windows_duplicate_startup_handle(stdin_handle,
                                          &ctx->duplicated_handles[0],
                                          err_out)
        || !windows_duplicate_startup_handle(stdout_handle,
                                             &ctx->duplicated_handles[1],
                                             err_out)
        || !windows_duplicate_startup_handle(stderr_handle,
                                             &ctx->duplicated_handles[2],
                                             err_out)) {
        windows_startup_context_free(ctx);
        return false;
    }

    ctx->info.StartupInfo.hStdInput  = ctx->duplicated_handles[0];
    ctx->info.StartupInfo.hStdOutput = ctx->duplicated_handles[1];
    ctx->info.StartupInfo.hStdError  = ctx->duplicated_handles[2];

    windows_add_inherited_handle(ctx, ctx->duplicated_handles[0]);
    windows_add_inherited_handle(ctx, ctx->duplicated_handles[1]);
    windows_add_inherited_handle(ctx, ctx->duplicated_handles[2]);

    if (ctx->inherited_count == 0) {
        return true;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    if (attr_size == 0) {
        set_windows_error_string(err_out,
                                 "InitializeProcThreadAttributeList failed",
                                 GetLastError());
        windows_startup_context_free(ctx);
        return false;
    }

    ctx->attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)ncc_alloc_size(1,
                                                                   attr_size);
    if (!InitializeProcThreadAttributeList(ctx->attributes, 1, 0,
                                           &attr_size)) {
        set_windows_error_string(err_out,
                                 "InitializeProcThreadAttributeList failed",
                                 GetLastError());
        windows_startup_context_free(ctx);
        return false;
    }

    ctx->info.lpAttributeList = ctx->attributes;

    if (!UpdateProcThreadAttribute(ctx->attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   ctx->inherited_handles,
                                   sizeof(HANDLE) * ctx->inherited_count,
                                   nullptr, nullptr)) {
        set_windows_error_string(err_out, "UpdateProcThreadAttribute failed",
                                 GetLastError());
        windows_startup_context_free(ctx);
        return false;
    }

    return true;
}

static void
take_windows_pump_error(ncc_process_result_t *out, windows_pump_ctx_t *ctx)
{
    if (!ctx || ctx->ok) {
        return;
    }

    if (out && !out->stderr_data) {
        out->stderr_data = ctx->err;
        ctx->err = nullptr;
    }

    ncc_free(ctx->err);
    ctx->err = nullptr;
}

static void
wait_thread_if_open(HANDLE *thread)
{
    if (thread && *thread) {
        WaitForSingleObject(*thread, INFINITE);
        close_thread_if_open(thread);
    }
}

static void
close_process_info(PROCESS_INFORMATION *pi)
{
    if (!pi) {
        return;
    }

    close_handle_if_open(&pi->hThread);
    close_handle_if_open(&pi->hProcess);
    pi->dwProcessId = 0;
    pi->dwThreadId  = 0;
}

bool
ncc_process_run(const ncc_process_spec_t *spec, ncc_process_result_t *out)
{
    process_result_init(out);

    if (!spec || !spec->program || !spec->program[0]) {
        set_error_string(out ? &out->stderr_data : nullptr,
                         "process spec missing program");
        return false;
    }

    const char *fallback_argv[] = {spec->program, nullptr};
    const char **argv = spec->argv ? spec->argv : fallback_argv;

    platform_verbose("launch stdin_bytes=%zu capture_stdout=%d capture_stderr=%d",
                     spec->stdin_len, spec->capture_stdout,
                     spec->capture_stderr);
    platform_verbose_argv("argv", argv);

    HANDLE stdin_parent  = nullptr;
    HANDLE stdin_child   = nullptr;
    HANDLE stdout_parent = nullptr;
    HANDLE stdout_child  = nullptr;
    HANDLE stderr_parent = nullptr;
    HANDLE stderr_child  = nullptr;

    PROCESS_INFORMATION pi = {0};
    windows_startup_context_t startup = {0};
    char    *application_path = nullptr;
    wchar_t *application_name = nullptr;
    wchar_t *command_line     = nullptr;
    ncc_buffer_t *stdout_buf  = nullptr;
    ncc_buffer_t *stderr_buf  = nullptr;
    HANDLE stdin_thread       = nullptr;
    HANDLE stdout_thread      = nullptr;
    HANDLE stderr_thread      = nullptr;
    windows_pump_ctx_t stdin_ctx = {0};
    windows_pump_ctx_t stdout_ctx = {0};
    windows_pump_ctx_t stderr_ctx = {0};
    bool success = false;

    if (spec->stdin_data
        && !create_child_pipe(&stdin_parent, &stdin_child, true,
                              out ? &out->stderr_data : nullptr)) {
        goto cleanup;
    }
    if (spec->capture_stdout
        && !create_child_pipe(&stdout_parent, &stdout_child, false,
                              out ? &out->stderr_data : nullptr)) {
        goto cleanup;
    }
    if (spec->capture_stderr
        && !create_child_pipe(&stderr_parent, &stderr_child, false,
                              out ? &out->stderr_data : nullptr)) {
        goto cleanup;
    }

    HANDLE std_input  = spec->stdin_data ? stdin_child : GetStdHandle(STD_INPUT_HANDLE);
    HANDLE std_output = spec->capture_stdout ? stdout_child : GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE std_error  = spec->capture_stderr ? stderr_child : GetStdHandle(STD_ERROR_HANDLE);

    application_path = windows_resolve_process_application(
        spec->program, out ? &out->stderr_data : nullptr);

    if (!application_path) {
        goto cleanup;
    }

    application_name = utf8_to_utf16(application_path);
    command_line     = build_windows_command_line(argv);

    if (!application_name || !command_line) {
        set_error_string(out ? &out->stderr_data : nullptr,
                         "failed to build process command");
        goto cleanup;
    }

    platform_verbose("application=%s", application_path);

    if (!windows_startup_context_init(&startup, std_input, std_output,
                                      std_error,
                                      out ? &out->stderr_data : nullptr)) {
        goto cleanup;
    }

    DWORD creation_flags = startup.attributes ? EXTENDED_STARTUPINFO_PRESENT : 0;
    BOOL  inherit_handles = startup.inherited_count > 0 ? TRUE : FALSE;
    BOOL ok = CreateProcessW(application_name, command_line, nullptr, nullptr,
                             inherit_handles, creation_flags, nullptr, nullptr,
                             &startup.info.StartupInfo, &pi);
    DWORD create_error = ok ? ERROR_SUCCESS : GetLastError();

    windows_startup_context_free(&startup);
    close_handle_if_open(&stdin_child);
    close_handle_if_open(&stdout_child);
    close_handle_if_open(&stderr_child);

    if (!ok) {
        set_windows_error_string(out ? &out->stderr_data : nullptr,
                                 "CreateProcessW failed", create_error);
        goto cleanup;
    }

    platform_verbose("started pid=%lu", (unsigned long)pi.dwProcessId);

    stdout_buf = spec->capture_stdout ? ncc_buffer_empty() : nullptr;
    stderr_buf = spec->capture_stderr ? ncc_buffer_empty() : nullptr;

    stdin_ctx = (windows_pump_ctx_t){
        .handle = stdin_parent,
        .data   = spec->stdin_data,
        .len    = spec->stdin_len,
        .ok     = true,
    };
    stdout_ctx = (windows_pump_ctx_t){
        .handle = stdout_parent,
        .buf    = stdout_buf,
        .ok     = true,
    };
    stderr_ctx = (windows_pump_ctx_t){
        .handle = stderr_parent,
        .buf    = stderr_buf,
        .ok     = true,
    };

    if (spec->capture_stdout) {
        if (!start_pump_thread(&stdout_thread, &stdout_ctx, read_pump_thread,
                               out ? &out->stderr_data : nullptr)) {
            TerminateProcess(pi.hProcess, 1);
            goto cleanup;
        }
        stdout_parent = nullptr;
    }
    if (spec->capture_stderr) {
        if (!start_pump_thread(&stderr_thread, &stderr_ctx, read_pump_thread,
                               out ? &out->stderr_data : nullptr)) {
            TerminateProcess(pi.hProcess, 1);
            goto cleanup;
        }
        stderr_parent = nullptr;
    }
    if (spec->stdin_data) {
        if (!start_pump_thread(&stdin_thread, &stdin_ctx, write_pump_thread,
                               out ? &out->stderr_data : nullptr)) {
            TerminateProcess(pi.hProcess, 1);
            goto cleanup;
        }
        stdin_parent = nullptr;
    }

    DWORD wait_rc = WaitForSingleObject(pi.hProcess, INFINITE);
    if (wait_rc == WAIT_FAILED) {
        set_windows_error_string(out ? &out->stderr_data : nullptr,
                                 "WaitForSingleObject failed",
                                 GetLastError());
        TerminateProcess(pi.hProcess, 1);
    }

    wait_thread_if_open(&stdin_thread);
    wait_thread_if_open(&stdout_thread);
    wait_thread_if_open(&stderr_thread);

    close_handle_if_open(&stdin_parent);
    close_handle_if_open(&stdout_parent);
    close_handle_if_open(&stderr_parent);

    take_windows_pump_error(out, &stdin_ctx);
    take_windows_pump_error(out, &stdout_ctx);
    take_windows_pump_error(out, &stderr_ctx);

    if (wait_rc == WAIT_FAILED || !stdin_ctx.ok || !stdout_ctx.ok
        || !stderr_ctx.ok) {
        goto cleanup;
    }

    if (spec->stdin_data) {
        platform_verbose("wrote %zu bytes to child stdin", spec->stdin_len);
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        set_windows_error_string(out ? &out->stderr_data : nullptr,
                                 "GetExitCodeProcess failed", GetLastError());
        goto cleanup;
    }

    size_t stdout_len = stdout_buf ? stdout_buf->byte_len : 0;
    size_t stderr_len = stderr_buf ? stderr_buf->byte_len : 0;
    platform_verbose("exit code=%lu stdout_bytes=%zu stderr_bytes=%zu",
                     (unsigned long)exit_code, stdout_len, stderr_len);

    if (out) {
        out->exit_code = (int)exit_code;
        if (stdout_buf) {
            out->stdout_len  = stdout_len;
            out->stdout_data = ncc_buffer_take(stdout_buf);
            stdout_buf = nullptr;
        }
        if (stderr_buf) {
            out->stderr_len  = stderr_len;
            out->stderr_data = ncc_buffer_take(stderr_buf);
            stderr_buf = nullptr;
        }
    }

    success = true;

cleanup:
    wait_thread_if_open(&stdin_thread);
    wait_thread_if_open(&stdout_thread);
    wait_thread_if_open(&stderr_thread);
    windows_startup_context_free(&startup);
    close_handle_if_open(&stdin_parent);
    close_handle_if_open(&stdin_child);
    close_handle_if_open(&stdout_parent);
    close_handle_if_open(&stdout_child);
    close_handle_if_open(&stderr_parent);
    close_handle_if_open(&stderr_child);
    close_process_info(&pi);
    ncc_buffer_free(stdout_buf);
    ncc_buffer_free(stderr_buf);
    ncc_free(application_path);
    ncc_free(application_name);
    ncc_free(command_line);
    return success;
}

#else

char *
ncc_platform_realpath(const char *path)
{
    if (!path || !path[0]) {
        return nullptr;
    }

    return realpath(path, nullptr);
}

char *
ncc_platform_get_exe_path(void)
{
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);

    char *buf = (char *)ncc_alloc_size(1, size + 1);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        ncc_free(buf);
        return nullptr;
    }

    char *resolved = realpath(buf, nullptr);
    ncc_free(buf);
    return resolved;
#elif defined(__linux__)
    return realpath("/proc/self/exe", nullptr);
#else
    return nullptr;
#endif
}

bool
ncc_platform_write_file(const char *path, const char *data, size_t len,
                        char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        set_error_string(err_out, "open failed: %s", strerror(errno));
        return false;
    }

    size_t written = fwrite(data, 1, len, f);
    int    close_rc = fclose(f);
    bool   ok       = written == len && close_rc == 0;

    if (!ok) {
        set_error_string(err_out, "write failed: %s", strerror(errno));
        return false;
    }

    return true;
}

bool
ncc_platform_path_eq(const char *lhs, const char *rhs)
{
    if (!lhs || !rhs) {
        return false;
    }

    char *l = ncc_platform_realpath(lhs);
    char *r = ncc_platform_realpath(rhs);

    if (!l || !r) {
        ncc_free(l);
        ncc_free(r);
        return false;
    }

    bool same = strcmp(l, r) == 0;
    ncc_free(l);
    ncc_free(r);
    return same;
}

char *
ncc_platform_temp_dir_create(const char *prefix)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) {
        tmp = "/tmp";
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf, "%s/%sXXXXXX", tmp,
                      (prefix && prefix[0]) ? prefix : "ncc_");

    char *templ = ncc_buffer_take(buf);
    if (!mkdtemp(templ)) {
        ncc_free(templ);
        return nullptr;
    }

    return templ;
}

bool
ncc_platform_remove_file(const char *path)
{
    if (!path) {
        return true;
    }

    return unlink(path) == 0 || errno == ENOENT;
}

bool
ncc_platform_remove_dir(const char *path)
{
    if (!path) {
        return true;
    }

    return rmdir(path) == 0 || errno == ENOENT;
}

static bool
ncc_platform_remove_tree(const char *path)
{
    if (!path || !path[0]) {
        return true;
    }

    struct stat st;
    if (lstat(path, &st) < 0) {
        return errno == ENOENT;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path) == 0 || errno == ENOENT;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return false;
    }

    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                ok = false;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0
            || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char *child = ncc_platform_join_path(path, entry->d_name);
        if (!ncc_platform_remove_tree(child)) {
            ok = false;
        }
        ncc_free(child);
    }

    if (closedir(dir) != 0) {
        ok = false;
    }

    return (rmdir(path) == 0 || errno == ENOENT) && ok;
}

static bool
set_fd_cloexec(int fd, char **err_out)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        set_error_string(err_out, "fcntl(F_GETFD) failed: %s",
                         strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        set_error_string(err_out, "fcntl(F_SETFD) failed: %s",
                         strerror(errno));
        return false;
    }

    return true;
}

static bool
set_fd_nonblocking(int fd, char **err_out)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        set_error_string(err_out, "fcntl(F_GETFL) failed: %s",
                         strerror(errno));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        set_error_string(err_out, "fcntl(F_SETFL) failed: %s",
                         strerror(errno));
        return false;
    }

    return true;
}

static void
close_fd_if_open(int *fd)
{
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static bool
pipe_with_cloexec(int fds[2], char **err_out)
{
    if (pipe(fds) < 0) {
        set_error_string(err_out, "pipe failed: %s", strerror(errno));
        return false;
    }

    if (!set_fd_cloexec(fds[0], err_out)
        || !set_fd_cloexec(fds[1], err_out)) {
        close_fd_if_open(&fds[0]);
        close_fd_if_open(&fds[1]);
        return false;
    }

    return true;
}

static void
close_process_pipes(int stdin_pipe[2], int stdout_pipe[2], int stderr_pipe[2],
                    int exec_pipe[2])
{
    close_fd_if_open(&stdin_pipe[0]);
    close_fd_if_open(&stdin_pipe[1]);
    close_fd_if_open(&stdout_pipe[0]);
    close_fd_if_open(&stdout_pipe[1]);
    close_fd_if_open(&stderr_pipe[0]);
    close_fd_if_open(&stderr_pipe[1]);
    close_fd_if_open(&exec_pipe[0]);
    close_fd_if_open(&exec_pipe[1]);
}

static void
close_unintended_child_fds(int keep_fd)
{
    long max_fd = sysconf(_SC_OPEN_MAX);

    if (max_fd < 0) {
        max_fd = 1024;
    }

    for (long fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
        if ((int)fd == keep_fd) {
            continue;
        }
        close((int)fd);
    }
}

static bool
ignore_sigpipe(struct sigaction *old_action, char **err_out)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGPIPE, &action, old_action) < 0) {
        set_error_string(err_out, "sigaction(SIGPIPE) failed: %s",
                         strerror(errno));
        return false;
    }

    return true;
}

static void
restore_sigpipe(const struct sigaction *old_action)
{
    if (old_action) {
        sigaction(SIGPIPE, old_action, nullptr);
    }
}

static void
write_exec_error_and_exit(int fd, int err)
{
    while (write(fd, &err, sizeof(err)) < 0 && errno == EINTR) {
    }
    _exit(127);
}

static bool
read_exec_error(int fd, int *err_out, char **msg_out)
{
    int     err = 0;
    ssize_t rc  = 0;

    do {
        rc = read(fd, &err, sizeof(err));
    } while (rc < 0 && errno == EINTR);

    if (rc < 0) {
        set_error_string(msg_out, "read exec status failed: %s",
                         strerror(errno));
        return false;
    }

    *err_out = rc > 0 ? err : 0;
    return true;
}

static bool
wait_for_child(pid_t pid, int *status_out, char **err_out)
{
    int status = 0;

    for (;;) {
        if (waitpid(pid, &status, 0) >= 0) {
            if (status_out) {
                *status_out = status;
            }
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        set_error_string(err_out, "waitpid failed: %s", strerror(errno));
        return false;
    }
}

static bool
drain_fd_ready(int *fd, ncc_buffer_t *buf, bool *still_open, char **err_out)
{
    char chunk[4096];

    while (*still_open) {
        ssize_t rc = read(*fd, chunk, sizeof(chunk));

        if (rc > 0) {
            ncc_buffer_append(buf, chunk, (size_t)rc);
            continue;
        }

        if (rc == 0) {
            close_fd_if_open(fd);
            *still_open = false;
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        set_error_string(err_out, "read failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool
write_fd_ready(int *fd, const char *data, size_t len, size_t *written,
               bool *still_open, char **err_out)
{
    while (*still_open && *written < len) {
        size_t remaining = len - *written;
        size_t chunk_len = ncc_min(remaining, (size_t)65536);
        ssize_t rc = write(*fd, data + *written, chunk_len);

        if (rc > 0) {
            *written += (size_t)rc;
            continue;
        }

        if (rc == 0) {
            set_error_string(err_out, "write failed: wrote zero bytes");
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        if (errno == EPIPE) {
            close_fd_if_open(fd);
            *still_open = false;
            return true;
        }

        set_error_string(err_out, "write failed: %s", strerror(errno));
        return false;
    }

    if (*still_open && *written == len) {
        close_fd_if_open(fd);
        *still_open = false;
    }

    return true;
}

typedef enum {
    PROCESS_STREAM_STDIN,
    PROCESS_STREAM_STDOUT,
    PROCESS_STREAM_STDERR,
} process_stream_role_t;

static bool
pump_child_io_posix(const ncc_process_spec_t *spec, int *stdin_fd,
                    int *stdout_fd, int *stderr_fd, ncc_buffer_t *stdout_buf,
                    ncc_buffer_t *stderr_buf, char **err_out)
{
    bool stdin_open  = stdin_fd && *stdin_fd >= 0;
    bool stdout_open = stdout_fd && *stdout_fd >= 0;
    bool stderr_open = stderr_fd && *stderr_fd >= 0;
    size_t stdin_written = 0;

    if (stdin_open && !set_fd_nonblocking(*stdin_fd, err_out)) {
        return false;
    }
    if (stdout_open && !set_fd_nonblocking(*stdout_fd, err_out)) {
        return false;
    }
    if (stderr_open && !set_fd_nonblocking(*stderr_fd, err_out)) {
        return false;
    }

    if (stdin_open && spec->stdin_len == 0) {
        close_fd_if_open(stdin_fd);
        stdin_open = false;
    }

    while (stdin_open || stdout_open || stderr_open) {
        struct pollfd        pfds[3];
        process_stream_role_t roles[3];
        nfds_t               nfds = 0;

        if (stdin_open) {
            pfds[nfds] = (struct pollfd){
                .fd     = *stdin_fd,
                .events = POLLOUT | POLLHUP | POLLERR,
            };
            roles[nfds++] = PROCESS_STREAM_STDIN;
        }
        if (stdout_open) {
            pfds[nfds] = (struct pollfd){
                .fd     = *stdout_fd,
                .events = POLLIN | POLLHUP | POLLERR,
            };
            roles[nfds++] = PROCESS_STREAM_STDOUT;
        }
        if (stderr_open) {
            pfds[nfds] = (struct pollfd){
                .fd     = *stderr_fd,
                .events = POLLIN | POLLHUP | POLLERR,
            };
            roles[nfds++] = PROCESS_STREAM_STDERR;
        }

        int rc = poll(pfds, nfds, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error_string(err_out, "poll failed: %s", strerror(errno));
            return false;
        }

        for (nfds_t i = 0; i < nfds; i++) {
            if (pfds[i].revents == 0) {
                continue;
            }

            if (roles[i] == PROCESS_STREAM_STDIN) {
                if ((pfds[i].revents & POLLOUT) == 0
                    && (pfds[i].revents & (POLLHUP | POLLERR)) != 0) {
                    close_fd_if_open(stdin_fd);
                    stdin_open = false;
                    continue;
                }
                if (!write_fd_ready(stdin_fd, spec->stdin_data,
                                    spec->stdin_len, &stdin_written,
                                    &stdin_open, err_out)) {
                    return false;
                }
            }
            else if (roles[i] == PROCESS_STREAM_STDOUT) {
                if (!drain_fd_ready(stdout_fd, stdout_buf, &stdout_open,
                                    err_out)) {
                    return false;
                }
            }
            else if (roles[i] == PROCESS_STREAM_STDERR) {
                if (!drain_fd_ready(stderr_fd, stderr_buf, &stderr_open,
                                    err_out)) {
                    return false;
                }
            }
        }
    }

    return true;
}

static void
cleanup_parent_process(pid_t pid, int stdin_pipe[2], int stdout_pipe[2],
                       int stderr_pipe[2], int exec_pipe[2],
                       ncc_buffer_t *stdout_buf, ncc_buffer_t *stderr_buf)
{
    close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
    wait_for_child(pid, nullptr, nullptr);
    ncc_buffer_free(stdout_buf);
    ncc_buffer_free(stderr_buf);
}

bool
ncc_process_run(const ncc_process_spec_t *spec, ncc_process_result_t *out)
{
    process_result_init(out);

    if (!spec || !spec->program || !spec->program[0]) {
        set_error_string(out ? &out->stderr_data : nullptr,
                         "process spec missing program");
        return false;
    }

    const char *fallback_argv[] = {spec->program, nullptr};
    const char **argv = spec->argv ? spec->argv : fallback_argv;

    platform_verbose("launch stdin_bytes=%zu capture_stdout=%d capture_stderr=%d",
                     spec->stdin_len, spec->capture_stdout,
                     spec->capture_stderr);
    platform_verbose_argv("argv", argv);

    int stdin_pipe[2]  = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    int exec_pipe[2]   = {-1, -1};

    if (spec->stdin_data
        && !pipe_with_cloexec(stdin_pipe,
                              out ? &out->stderr_data : nullptr)) {
        return false;
    }
    if (spec->capture_stdout
        && !pipe_with_cloexec(stdout_pipe,
                              out ? &out->stderr_data : nullptr)) {
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        return false;
    }
    if (spec->capture_stderr
        && !pipe_with_cloexec(stderr_pipe,
                              out ? &out->stderr_data : nullptr)) {
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        return false;
    }
    if (!pipe_with_cloexec(exec_pipe, out ? &out->stderr_data : nullptr)) {
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        set_error_string(out ? &out->stderr_data : nullptr,
                         "fork failed: %s", strerror(errno));
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        return false;
    }

    if (pid == 0) {
        close_fd_if_open(&exec_pipe[0]);
        if (spec->stdin_data) {
            close_fd_if_open(&stdin_pipe[1]);
            if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
                write_exec_error_and_exit(exec_pipe[1], errno);
            }
            close_fd_if_open(&stdin_pipe[0]);
        }
        if (spec->capture_stdout) {
            close_fd_if_open(&stdout_pipe[0]);
            if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
                write_exec_error_and_exit(exec_pipe[1], errno);
            }
            close_fd_if_open(&stdout_pipe[1]);
        }
        if (spec->capture_stderr) {
            close_fd_if_open(&stderr_pipe[0]);
            if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
                write_exec_error_and_exit(exec_pipe[1], errno);
            }
            close_fd_if_open(&stderr_pipe[1]);
        }

        close_unintended_child_fds(exec_pipe[1]);
        execvp(spec->program, (char *const *)argv);
        write_exec_error_and_exit(exec_pipe[1], errno);
    }

    close_fd_if_open(&exec_pipe[1]);

    if (spec->stdin_data) {
        close_fd_if_open(&stdin_pipe[0]);
    }
    if (spec->capture_stdout) {
        close_fd_if_open(&stdout_pipe[1]);
    }
    if (spec->capture_stderr) {
        close_fd_if_open(&stderr_pipe[1]);
    }

    int exec_err = 0;
    if (!read_exec_error(exec_pipe[0], &exec_err,
                         out ? &out->stderr_data : nullptr)) {
        close_fd_if_open(&exec_pipe[0]);
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        wait_for_child(pid, nullptr, nullptr);
        return false;
    }
    close_fd_if_open(&exec_pipe[0]);

    if (exec_err != 0) {
        set_error_string(out ? &out->stderr_data : nullptr,
                         "exec %s failed: %s", spec->program,
                         strerror(exec_err));
        close_process_pipes(stdin_pipe, stdout_pipe, stderr_pipe, exec_pipe);
        wait_for_child(pid, nullptr, nullptr);
        return false;
    }

    platform_verbose("started pid=%ld", (long)pid);

    ncc_buffer_t *stdout_buf = spec->capture_stdout ? ncc_buffer_empty() : nullptr;
    ncc_buffer_t *stderr_buf = spec->capture_stderr ? ncc_buffer_empty() : nullptr;

    struct sigaction old_pipe_action;
    bool             sigpipe_ignored = false;

    if (spec->stdin_data) {
        if (!ignore_sigpipe(&old_pipe_action,
                            out ? &out->stderr_data : nullptr)) {
            cleanup_parent_process(pid, stdin_pipe, stdout_pipe, stderr_pipe,
                                   exec_pipe, stdout_buf, stderr_buf);
            return false;
        }
        sigpipe_ignored = true;
    }

    bool pump_ok = pump_child_io_posix(spec, &stdin_pipe[1], &stdout_pipe[0],
                                       &stderr_pipe[0], stdout_buf,
                                       stderr_buf,
                                       out ? &out->stderr_data : nullptr);

    if (sigpipe_ignored) {
        restore_sigpipe(&old_pipe_action);
    }

    if (!pump_ok) {
        cleanup_parent_process(pid, stdin_pipe, stdout_pipe, stderr_pipe,
                               exec_pipe, stdout_buf, stderr_buf);
        return false;
    }

    if (spec->stdin_data) {
        platform_verbose("wrote %zu bytes to child stdin", spec->stdin_len);
    }

    int status = 0;
    if (!wait_for_child(pid, &status, out ? &out->stderr_data : nullptr)) {
        ncc_buffer_free(stdout_buf);
        ncc_buffer_free(stderr_buf);
        return false;
    }

    int exit_code = 1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }

    size_t stdout_len = stdout_buf ? stdout_buf->byte_len : 0;
    size_t stderr_len = stderr_buf ? stderr_buf->byte_len : 0;
    platform_verbose("exit code=%d stdout_bytes=%zu stderr_bytes=%zu",
                     exit_code, stdout_len, stderr_len);

    if (out) {
        out->exit_code = exit_code;
        if (stdout_buf) {
            out->stdout_len  = stdout_len;
            out->stdout_data = ncc_buffer_take(stdout_buf);
        }
        if (stderr_buf) {
            out->stderr_len  = stderr_len;
            out->stderr_data = ncc_buffer_take(stderr_buf);
        }
    } else {
        ncc_buffer_free(stdout_buf);
        ncc_buffer_free(stderr_buf);
    }

    return true;
}

#endif

bool
ncc_temp_workspace_create(ncc_temp_workspace_t *workspace, const char *prefix,
                          char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    if (!workspace) {
        set_error_string(err_out, "temp workspace output is null");
        return false;
    }

    workspace->path = nullptr;
    workspace->path = ncc_platform_temp_dir_create(prefix);
    if (!workspace->path) {
        set_error_string(err_out, "failed to create temporary directory");
        return false;
    }

    return true;
}

const char *
ncc_temp_workspace_path(const ncc_temp_workspace_t *workspace)
{
    return workspace ? workspace->path : nullptr;
}

char *
ncc_temp_workspace_join(const ncc_temp_workspace_t *workspace,
                        const char *leaf)
{
    if (!workspace || !workspace->path) {
        return nullptr;
    }

    return ncc_platform_join_path(workspace->path, leaf);
}

void
ncc_temp_workspace_cleanup(ncc_temp_workspace_t *workspace)
{
    if (!workspace || !workspace->path) {
        return;
    }

    ncc_platform_remove_tree(workspace->path);
    ncc_free(workspace->path);
    workspace->path = nullptr;
}
