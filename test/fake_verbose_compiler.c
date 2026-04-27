#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define LARGE_OUTPUT_SIZE (2u * 1024u * 1024u)

static void
set_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

static const char *
arg_after(int argc, char **argv, const char *needle)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], needle) == 0) {
            return argv[i + 1];
        }
    }

    return nullptr;
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

    return 0;
}

static int
run_preprocessor(const char *output)
{
    FILE *f = output ? fopen(output, "wb") : stdout;
    if (!f) {
        return 3;
    }

    fputs("const char *payload = \"", f);
    if (write_repeated(f, 'a', LARGE_OUTPUT_SIZE) != 0) {
        if (output) {
            fclose(f);
        }
        return 4;
    }
    fputs("\";\nint main(void) { return payload[0] == 'a' ? 0 : 1; }\n", f);

    if (output) {
        return fclose(f) == 0 ? 0 : 5;
    }
    return fflush(f) == 0 ? 0 : 5;
}

static int
run_compiler(const char *output)
{
    char chunk[4096];

    set_binary_stdio();
    if (write_repeated(stdout, 'V', LARGE_OUTPUT_SIZE) != 0
        || fflush(stdout) != 0) {
        return 6;
    }

    while (fread(chunk, 1, sizeof(chunk), stdin) > 0) {
    }
    if (ferror(stdin)) {
        return 7;
    }

    if (output) {
        FILE *f = fopen(output, "wb");
        if (!f) {
            return 8;
        }
        fputs("fake object\n", f);
        if (fclose(f) != 0) {
            return 9;
        }
    }

    return 0;
}

int
main(int argc, char **argv)
{
    const char *output = arg_after(argc, argv, "-o");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-E") == 0) {
            return run_preprocessor(output);
        }
    }

    return run_compiler(output);
}
