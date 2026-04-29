#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/c_tokenizer.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "internal/parse/grammar_internal.h"

static int test_count = 0;
static int fail_count = 0;

#define TEST(name) \
    do { test_count++; printf("  test %d: %s ... ", test_count, name); } while (0)
#define PASS() \
    do { printf("PASS\n"); } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); fail_count++; } while (0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

static ncc_grammar_t *
load_c_grammar(void)
{
    const char *paths[] = {
        "c_ncc.bnf",
        "../c_ncc.bnf",
        "../../c_ncc.bnf",
        nullptr,
    };

    const char *srcroot = getenv("MESON_SOURCE_ROOT");
    FILE       *f       = nullptr;

    for (const char **p = paths; *p; p++) {
        f = fopen(*p, "r");
        if (f) {
            break;
        }
    }

    if (!f && srcroot) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/c_ncc.bnf", srcroot);
        f = fopen(path, "r");
    }

    if (!f) {
        fprintf(stderr, "  [SKIP] Cannot find c_ncc.bnf\n");
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char  *buf   = ncc_alloc_size(1, (size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    ncc_string_t bnf_text = ncc_string_from_raw(buf, (int64_t)nread);
    ncc_free(buf);

    ncc_grammar_t *g = ncc_grammar_new();
    ncc_grammar_set_error_recovery(g, false);

    ncc_string_t start = NCC_STRING_STATIC("translation_unit");
    bool         ok    = ncc_bnf_load(bnf_text, start, g);

    if (!ok) {
        fprintf(stderr, "  [FAIL] ncc_bnf_load failed for c_ncc.bnf\n");
        ncc_grammar_free(g);
        return nullptr;
    }

    g->tokenize_cb = (void *)ncc_c_tokenize;
    return g;
}

static bool
token_text_eq(ncc_token_info_t *tok, const char *text)
{
    if (!tok || !ncc_option_is_set(tok->value)) {
        return false;
    }

    ncc_string_t value = ncc_option_get(tok->value);
    size_t       len   = strlen(text);

    return value.data && value.u8_bytes == len
        && memcmp(value.data, text, len) == 0;
}

static bool
stream_contains_token_text(ncc_token_stream_t *ts, const char *text)
{
    for (int32_t i = 0; i < ts->token_count; i++) {
        if (token_text_eq(ts->tokens[i], text)) {
            return true;
        }
    }

    return false;
}

typedef struct {
    const char *name;
    const char *source;
    const char *expected_tokens[8];
} msvc_parse_case_t;

static void
run_msvc_parse_case(const msvc_parse_case_t *tc)
{
    TEST(tc->name);
    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_buffer_t *buf =
        ncc_buffer_from_bytes(tc->source, (int64_t)strlen(tc->source));
    ncc_c_tokenizer_state_t *state = ncc_c_tokenizer_state_new();
    ncc_scanner_t *scanner = ncc_scanner_new(
        buf, ncc_c_tokenize, g,
        ncc_option_set(ncc_string_t, ncc_string_from_cstr("test.c")),
        state, ncc_c_tokenizer_reset);
    ncc_token_stream_t *ts = ncc_token_stream_new(scanner);

    while (ncc_stream_next(ts)) {
    }

    CHECK(ts->token_count > 0, "token stream is empty");
    for (int i = 0; tc->expected_tokens[i]; i++) {
        CHECK(stream_contains_token_text(ts, tc->expected_tokens[i]),
              "expected MSVC token missing from token stream");
    }

    ncc_stream_reset(ts);

    ncc_pwz_parser_t *parser = ncc_pwz_new(g);
    CHECK(parser != nullptr, "parser allocation failed");
    CHECK(ncc_pwz_parse(parser, ts), "parser should accept MSVC-style case");
    CHECK(ncc_pwz_get_tree(parser) != nullptr, "parser returned no tree");

    PASS();
    ncc_pwz_free(parser);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_buffer_free(buf);
    ncc_grammar_free(g);
}

static void
test_msvc_extensions_parse(void)
{
    static const msvc_parse_case_t cases[] = {
        {
            .name = "msvc __int spellings parse",
            .source =
                "typedef signed __int8 i8;\n"
                "typedef unsigned __int16 u16;\n"
                "typedef signed __int32 i32;\n"
                "typedef unsigned __int64 u64;\n"
                "typedef signed __int128 i128;\n"
                "typedef __int128_t i128_alias;\n"
                "int main(void) { return 0; }\n",
            .expected_tokens = {
                "__int8", "__int16", "__int32", "__int64",
                "__int128", "__int128_t", nullptr,
            },
        },
        {
            .name = "msvc calling conventions parse",
            .source =
                "int __cdecl cdecl_sum(int x, int y);\n"
                "int __fastcall fastcall_sum(int x, int y);\n"
                "int __stdcall stdcall_sum(int x, int y);\n"
                "int __thiscall thiscall_sum(int x, int y);\n"
                "int __vectorcall vectorcall_sum(int x, int y);\n"
                "int main(void) { return 0; }\n",
            .expected_tokens = {
                "__cdecl", "__fastcall", "__stdcall", "__thiscall",
                "__vectorcall", nullptr,
            },
        },
        {
            .name = "msvc function pointer calling convention parse",
            .source =
                "typedef int (__stdcall *cb)(int);\n"
                "int call_cb(int (__stdcall *)(int));\n"
                "int main(void) { return 0; }\n",
            .expected_tokens = {
                "__stdcall", nullptr,
            },
        },
        {
            .name = "msvc declspec and forceinline parse",
            .source =
                "__declspec(noreturn) void panic(void);\n"
                "__forceinline int fast_inline(void) { return 1; }\n"
                "int main(void) { return fast_inline(); }\n",
            .expected_tokens = {
                "__declspec", "__forceinline", nullptr,
            },
        },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_msvc_parse_case(&cases[i]);
    }
}

int
main(void)
{
    printf("Tokenizer regression tests\n");

    test_msvc_extensions_parse();

    if (fail_count) {
        printf("\nFAILURES: %d/%d\n", fail_count, test_count);
        return 1;
    }

    printf("\nAll %d tokenizer tests passed.\n", test_count);
    return 0;
}
