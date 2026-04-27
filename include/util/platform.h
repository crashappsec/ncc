#pragma once

#include "ncc.h"

typedef struct {
    const char *program;
    const char **argv;
    const char *stdin_data;
    size_t stdin_len;
    bool capture_stdout;
    bool capture_stderr;
} ncc_process_spec_t;

typedef struct {
    int exit_code;
    char *stdout_data;
    size_t stdout_len;
    char *stderr_data;
    size_t stderr_len;
} ncc_process_result_t;

/*
 * Runs a child process and waits for it to exit.
 *
 * spec->program is the executable to launch. If spec->argv is null, the child
 * receives a single argv[0] equal to spec->program. If spec->stdin_data is not
 * null, exactly spec->stdin_len bytes are written to child stdin. If
 * capture_stdout or capture_stderr is true, that stream is captured as bytes;
 * otherwise the child inherits the corresponding parent stream.
 *
 * Returns false for invalid specs, OS setup failures, launch/exec failures,
 * and fatal parent-side I/O failures. In that case out->exit_code remains -1
 * and out->stderr_data may contain a diagnostic owned by the caller. Returns
 * true only after the child process has launched successfully; child exit
 * status is reported through out->exit_code even when it is nonzero. If the
 * child closes stdin before all requested input is written, the child result
 * is still reported instead of treating the closed pipe as a launch failure.
 *
 * Captured stdout/stderr buffers are byte buffers owned by the caller and
 * released by ncc_process_result_free(). A capture buffer may contain embedded
 * NUL bytes and may not end in a newline; use stdout_len/stderr_len rather than
 * strlen() when replaying or inspecting captured output. When a capture flag is
 * false, the matching pointer is null and length is zero.
 */
bool ncc_process_run(const ncc_process_spec_t *spec, ncc_process_result_t *out);
void ncc_process_result_free(ncc_process_result_t *out);

typedef struct {
    char *path;
} ncc_temp_workspace_t;

/*
 * Path helpers that return char * return caller-owned strings released with
 * ncc_free(). They return nullptr when the requested path cannot be resolved,
 * created, encoded, or is unsupported on the current platform.
 *
 * ncc_platform_dirname() returns a caller-owned directory component and uses
 * "." for null, empty, or basename-only inputs. ncc_platform_join_path()
 * returns a caller-owned joined path; null or empty components are treated as
 * empty path parts rather than errors.
 *
 * ncc_platform_temp_dir_create() creates a real directory and returns its
 * caller-owned path. The caller is responsible for removing the directory with
 * ncc_platform_remove_dir() or another recursive cleanup path and then freeing
 * the returned string.
 */
char *ncc_platform_get_exe_path(void);
char *ncc_platform_realpath(const char *path);
char *ncc_platform_dirname(const char *path);
char *ncc_platform_temp_dir_create(const char *prefix);
char *ncc_platform_join_path(const char *dir, const char *leaf);

/*
 * Writes exactly len bytes from data to path, replacing any existing file.
 * Returns false on open, write, close, path-encoding, or OS errors. If err_out
 * is non-null, it is reset to nullptr on entry and receives a caller-owned
 * diagnostic string on failure.
 */
bool ncc_platform_write_file(const char *path, const char *data, size_t len,
                             char **err_out);

/*
 * ncc_platform_path_eq() compares resolved paths and returns false if either
 * side is null or cannot be resolved. The remove helpers are cleanup-oriented:
 * null paths and already-missing files/directories are treated as success.
 */
bool ncc_platform_path_eq(const char *lhs, const char *rhs);
bool ncc_platform_remove_file(const char *path);
bool ncc_platform_remove_dir(const char *path);

/*
 * Owns a private temporary directory and removes it recursively during cleanup.
 * The path returned by ncc_temp_workspace_path() and the workspace->path member
 * are owned by the workspace. Paths returned by ncc_temp_workspace_join() are
 * caller-owned strings.
 *
 * ncc_temp_workspace_create() initializes workspace->path, resets err_out to
 * nullptr when provided, and returns false with a caller-owned err_out
 * diagnostic if the output pointer is null or the directory cannot be created.
 * ncc_temp_workspace_path() returns a borrowed pointer valid until cleanup.
 * ncc_temp_workspace_join() returns nullptr if the workspace is not initialized.
 * ncc_temp_workspace_cleanup() is idempotent and nulls workspace->path.
 */
bool ncc_temp_workspace_create(ncc_temp_workspace_t *workspace,
                               const char *prefix, char **err_out);
const char *ncc_temp_workspace_path(const ncc_temp_workspace_t *workspace);
char *ncc_temp_workspace_join(const ncc_temp_workspace_t *workspace,
                              const char *leaf);
void ncc_temp_workspace_cleanup(ncc_temp_workspace_t *workspace);
