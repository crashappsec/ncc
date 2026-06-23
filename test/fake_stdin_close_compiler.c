#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define LARGE_SOURCE_SIZE (2u * 1024u * 1024u)
#define FINAL_COMPILER_MESSAGE "fake final compiler received temp source file\n"
#define FINAL_COMPILER_BAD_SOURCE \
    "fake final compiler expected temp source file after -x c\n"
#define FINAL_COMPILER_EXIT 42

static void
set_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

static bool
has_arg(int argc, char **argv, const char *needle)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], needle) == 0) {
            return true;
        }
    }

    return false;
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
run_preprocessor(void)
{
    fputs("const char payload[] = \"", stdout);
    if (write_repeated(stdout, 'a', LARGE_SOURCE_SIZE) != 0) {
        return 2;
    }
    fputs("\";\nint main(void) { return payload[0] == 'a' ? 0 : 1; }\n",
          stdout);

    return fflush(stdout) == 0 ? 0 : 3;
}

static const char *
find_c_source_arg(int argc, char **argv)
{
    for (int i = 1; i + 2 < argc; i++) {
        if (strcmp(argv[i], "-x") == 0 && strcmp(argv[i + 1], "c") == 0) {
            return argv[i + 2];
        }
    }

    return nullptr;
}

int
main(int argc, char **argv)
{
    set_binary_stdio();

    if (has_arg(argc, argv, "-E")) {
        return run_preprocessor();
    }

    const char *source_arg = find_c_source_arg(argc, argv);
    if (!source_arg || strcmp(source_arg, "-") == 0) {
        fputs(FINAL_COMPILER_BAD_SOURCE, stderr);
        return 43;
    }

    FILE *source = fopen(source_arg, "rb");
    if (!source) {
        fputs(FINAL_COMPILER_BAD_SOURCE, stderr);
        return 44;
    }
    fclose(source);

    fputs(FINAL_COMPILER_MESSAGE, stderr);
    fflush(stderr);
    return FINAL_COMPILER_EXIT;
}
