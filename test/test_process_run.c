#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "util/platform.h"

#define LARGE_PAYLOAD_SIZE (2u * 1024u * 1024u)
#define EARLY_STDIN_CLOSE_MESSAGE "child closed stdin before reading\n"
#define EARLY_STDIN_CLOSE_EXIT 42
#define FD_LEAK_MARKER "ncc-process-run-fd-leak-marker"
#define FD_LEAK_EXIT 77

static void
set_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

static int
write_repeated(FILE *stream, int ch, size_t len)
{
    char chunk[4096];
    memset(chunk, ch, sizeof(chunk));

    while (len > 0) {
        size_t n = len < sizeof(chunk) ? len : sizeof(chunk);
        if (fwrite(chunk, 1, n, stream) != n) {
            return 1;
        }
        len -= n;
    }

    return fflush(stream) == 0 ? 0 : 1;
}

static int
child_early_stdout(void)
{
    char chunk[4096];

    set_binary_stdio();
    if (write_repeated(stdout, 'O', LARGE_PAYLOAD_SIZE) != 0) {
        return 2;
    }

    while (fread(chunk, 1, sizeof(chunk), stdin) > 0) {
    }

    return ferror(stdin) ? 3 : 0;
}

static int
child_exit_immediately(void)
{
    set_binary_stdio();
    fputs(EARLY_STDIN_CLOSE_MESSAGE, stderr);
    fflush(stderr);
    return EARLY_STDIN_CLOSE_EXIT;
}

static int
child_check_argv(int argc, char **argv)
{
    static const char *expected[] = {
        "--child-check-argv",
        "space arg",
        "quote \"inner\" arg",
        "",
        "trailing\\",
        "dir with space\\",
        "slashes\\\\quote\"",
        nullptr,
    };

    int expected_argc = 0;
    while (expected[expected_argc]) {
        expected_argc++;
    }

    if (argc != expected_argc + 1) {
        fprintf(stderr, "argc mismatch: expected %d got %d\n",
                expected_argc + 1, argc);
        return 43;
    }

    for (int i = 0; expected[i]; i++) {
        const char *actual = argv[i + 1] ? argv[i + 1] : "(null)";
        if (strcmp(actual, expected[i]) != 0) {
            fprintf(stderr, "argv[%d] mismatch: expected <%s> got <%s>\n",
                    i + 1, expected[i], actual);
            return 44;
        }
    }

    return 0;
}

#ifndef _WIN32
static int
child_check_fd_leak(void)
{
    char buf[128];

    for (int fd = 3; fd < 128; fd++) {
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0) {
            continue;
        }

        off_t old_pos = lseek(fd, 0, SEEK_CUR);
        if (lseek(fd, 0, SEEK_SET) < 0) {
            continue;
        }

        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (old_pos >= 0) {
            (void)lseek(fd, old_pos, SEEK_SET);
        }
        if (n <= 0) {
            continue;
        }

        buf[n] = '\0';
        if (strstr(buf, FD_LEAK_MARKER)) {
            printf("LEAK_CONFIRMED fd=%d\n", fd);
            return FD_LEAK_EXIT;
        }
    }

    return 0;
}
#endif

static char *
make_payload(size_t len)
{
    char *payload = malloc(len ? len : 1);
    if (!payload) {
        return nullptr;
    }

    memset(payload, 'I', len);
    return payload;
}

static bool
expect(bool cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }

    return true;
}

static bool
test_missing_executable(void)
{
    const char *argv[] = {"ncc-definitely-missing-process-run", nullptr};
    ncc_process_spec_t spec = {
        .program        = argv[0],
        .argv           = argv,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    bool ok = ncc_process_run(&spec, &result);
    bool pass = true;

    pass &= expect(!ok, "missing executable should be a launch failure");
    pass &= expect(result.exit_code == -1,
                   "missing executable should not report a child exit code");
    pass &= expect(result.stderr_data && result.stderr_data[0],
                   "missing executable should produce an error message");

    ncc_process_result_free(&result);
    return pass;
}

static bool
test_captured_output_during_stdin(const char *self)
{
    char *payload = make_payload(LARGE_PAYLOAD_SIZE);
    if (!payload) {
        fprintf(stderr, "FAIL: failed to allocate stdin payload\n");
        return false;
    }

    const char *argv[] = {self, "--child-early-stdout", nullptr};
    ncc_process_spec_t spec = {
        .program        = self,
        .argv           = argv,
        .stdin_data     = payload,
        .stdin_len      = LARGE_PAYLOAD_SIZE,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    bool ok = ncc_process_run(&spec, &result);
    bool pass = true;

    pass &= expect(ok, "early-output child should run successfully");
    pass &= expect(result.exit_code == 0,
                   "early-output child should exit successfully");
    pass &= expect(result.stdout_data != nullptr,
                   "early-output child stdout should be captured");
    pass &= expect(result.stdout_len == LARGE_PAYLOAD_SIZE,
                   "captured stdout should contain the full payload");

    ncc_process_result_free(&result);
    free(payload);
    return pass;
}

static bool
test_early_stdin_close(const char *self)
{
    char *payload = make_payload(LARGE_PAYLOAD_SIZE);
    if (!payload) {
        fprintf(stderr, "FAIL: failed to allocate stdin payload\n");
        return false;
    }

    const char *argv[] = {self, "--child-exit-immediately", nullptr};
    ncc_process_spec_t spec = {
        .program        = self,
        .argv           = argv,
        .stdin_data     = payload,
        .stdin_len      = LARGE_PAYLOAD_SIZE,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    bool ok = ncc_process_run(&spec, &result);
    bool pass = true;

    pass &= expect(ok, "early stdin close should preserve the child result");
    pass &= expect(result.exit_code == EARLY_STDIN_CLOSE_EXIT,
                   "early stdin close should report the child exit status");
    pass &= expect(result.stderr_data
                       && strstr(result.stderr_data,
                                 EARLY_STDIN_CLOSE_MESSAGE) != nullptr,
                   "early stdin close should preserve child stderr");

    ncc_process_result_free(&result);
    free(payload);
    return pass;
}

static bool
test_tricky_argv_round_trip(const char *self)
{
    const char *argv[] = {
        self,
        "--child-check-argv",
        "space arg",
        "quote \"inner\" arg",
        "",
        "trailing\\",
        "dir with space\\",
        "slashes\\\\quote\"",
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = self,
        .argv           = argv,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    bool ok = ncc_process_run(&spec, &result);
    bool pass = true;

    pass &= expect(ok, "tricky argv child should launch");
    pass &= expect(result.exit_code == 0,
                   "tricky argv child should receive exact arguments");
    if (!pass && result.stderr_data) {
        fputs(result.stderr_data, stderr);
    }

    ncc_process_result_free(&result);
    return pass;
}

static bool
test_unrelated_fd_not_inherited(const char *self)
{
#ifdef _WIN32
    (void)self;
    return true;
#else
    char path[] = "/tmp/ncc-fd-leak-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "FAIL: mkstemp failed: %s\n", strerror(errno));
        return false;
    }
    unlink(path);

    size_t marker_len = strlen(FD_LEAK_MARKER);
    if (write(fd, FD_LEAK_MARKER, marker_len) != (ssize_t)marker_len
        || lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "FAIL: failed to prepare marker fd: %s\n",
                strerror(errno));
        close(fd);
        return false;
    }

    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) {
        fprintf(stderr, "FAIL: failed to clear FD_CLOEXEC: %s\n",
                strerror(errno));
        close(fd);
        return false;
    }

    const char *argv[] = {self, "--child-check-fd-leak", nullptr};
    ncc_process_spec_t spec = {
        .program        = self,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    bool ok = ncc_process_run(&spec, &result);
    bool pass = true;

    pass &= expect(ok, "fd leak probe child should launch");
    pass &= expect(result.exit_code == 0,
                   "child should not inherit unrelated marker fd");
    pass &= expect(!result.stdout_data
                       || strstr(result.stdout_data,
                                 "LEAK_CONFIRMED") == nullptr,
                   "child reported inherited marker fd");

    if (!pass && result.stdout_data) {
        fputs(result.stdout_data, stderr);
    }
    if (!pass && result.stderr_data) {
        fputs(result.stderr_data, stderr);
    }

    ncc_process_result_free(&result);
    close(fd);
    return pass;
#endif
}

int
main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--child-early-stdout") == 0) {
        return child_early_stdout();
    }
    if (argc > 1 && strcmp(argv[1], "--child-exit-immediately") == 0) {
        return child_exit_immediately();
    }
    if (argc > 1 && strcmp(argv[1], "--child-check-argv") == 0) {
        return child_check_argv(argc, argv);
    }
#ifndef _WIN32
    if (argc > 1 && strcmp(argv[1], "--child-check-fd-leak") == 0) {
        return child_check_fd_leak();
    }
#endif

    bool pass = true;

    pass &= test_missing_executable();
    pass &= test_captured_output_during_stdin(argv[0]);
    pass &= test_early_stdin_close(argv[0]);
    pass &= test_tricky_argv_round_trip(argv[0]);
    pass &= test_unrelated_fd_not_inherited(argv[0]);

    if (!pass) {
        return 1;
    }

    printf("process-runner tests passed\n");
    return 0;
}
