// test_rstr_ansi.c — Tests for ncc_string_to_ansi() rendering.

#include "ncc_runtime.h"
#include "lib/alloc.h"
#include <stdio.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

#define TEST(name) \
    do { test_count++; printf("  test %d: %s ... ", test_count, name); } while (0)
#define PASS() \
    do { printf("PASS\n"); } while (0)
#define FAIL(msg, ...) \
    do { printf("FAIL: " msg "\n", ##__VA_ARGS__); fail_count++; } while (0)
#define CHECK(cond, msg, ...) \
    do { if (!(cond)) { FAIL(msg, ##__VA_ARGS__); return; } } while (0)

static void
test_plain_no_escapes(void)
{
    TEST("plain string produces no escapes");

    ncc_string_t *s = r"Hello world";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    CHECK(strcmp(ansi, "Hello world") == 0,
          "expected 'Hello world', got '%s'", ansi);
    ncc_free(ansi);
    PASS();
}

static void
test_bold(void)
{
    TEST("bold produces SGR 1");

    ncc_string_t *s = r"«b»hi«/b»";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    // Should contain ESC[0;1m somewhere and end with ESC[0m
    CHECK(strstr(ansi, "\e[0;1m") != nullptr,
          "missing bold SGR, got: '%s'", ansi);
    // The reset should appear
    char *last_reset = strstr(ansi, "\e[0m");
    CHECK(last_reset != nullptr, "missing reset");
    ncc_free(ansi);
    PASS();
}

static void
test_italic(void)
{
    TEST("italic produces SGR 3");

    ncc_string_t *s = r"«i»hey«/i»";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    CHECK(strstr(ansi, "\e[0;3m") != nullptr,
          "missing italic SGR, got: '%s'", ansi);
    ncc_free(ansi);
    PASS();
}

static void
test_multiple_styles(void)
{
    TEST("bold then italic");

    ncc_string_t *s = r"«b»A«/b»«i»B«/i»";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    CHECK(strstr(ansi, ";1m") != nullptr,
          "missing bold, got: '%s'", ansi);
    CHECK(strstr(ansi, ";3m") != nullptr,
          "missing italic, got: '%s'", ansi);
    // Both chars should appear
    char *a = strchr(ansi, 'A');
    char *b = strchr(ansi, 'B');
    CHECK(a != nullptr && b != nullptr, "missing text content");
    CHECK(a < b, "A should come before B");
    ncc_free(ansi);
    PASS();
}

// Extract visible text from ANSI string (skip escape sequences).
static char *
strip_ansi(const char *s)
{
    size_t len = strlen(s);
    char  *out = (char *)ncc_alloc_size(1, len + 1);
    size_t j   = 0;

    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\e' && i + 1 < len && s[i + 1] == '[') {
            // Skip until 'm'.
            i += 2;
            while (i < len && s[i] != 'm') {
                i++;
            }
        }
        else {
            out[j++] = s[i];
        }
    }

    out[j] = '\0';
    return out;
}

static void
test_uppercase(void)
{
    TEST("upper case transform");

    ncc_string_t *s = r"«upper»hello«/upper»";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    char *text = strip_ansi(ansi);
    CHECK(strcmp(text, "HELLO") == 0,
          "expected HELLO, got: '%s'", text);
    ncc_free(text);
    ncc_free(ansi);
    PASS();
}

static void
test_lowercase(void)
{
    TEST("lower case transform");

    ncc_string_t *s = r"«lower»HELLO«/lower»";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    char *text = strip_ansi(ansi);
    CHECK(strcmp(text, "hello") == 0,
          "expected hello, got: '%s'", text);
    ncc_free(text);
    ncc_free(ansi);
    PASS();
}

static void
test_null_input(void)
{
    TEST("null input returns null");

    char *ansi = ncc_string_to_ansi(nullptr);
    CHECK(ansi == nullptr, "expected null return");
    PASS();
}

static void
test_bracket_syntax(void)
{
    TEST("bracket syntax bold");

    ncc_string_t *s = r"[|b|]yo[|/b|]";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    CHECK(strstr(ansi, ";1m") != nullptr,
          "missing bold SGR, got: '%s'", ansi);
    CHECK(strstr(ansi, "yo") != nullptr,
          "missing text content");
    ncc_free(ansi);
    PASS();
}

static void
test_unclosed_style(void)
{
    TEST("unclosed style extends to end");

    ncc_string_t *s = r"«b»forever";
    char *ansi = ncc_string_to_ansi(s);

    CHECK(ansi != nullptr, "returned null");
    CHECK(strstr(ansi, ";1m") != nullptr,
          "missing bold SGR, got: '%s'", ansi);
    CHECK(strstr(ansi, "forever") != nullptr,
          "missing text content");
    // Should end with a reset
    size_t len = strlen(ansi);
    CHECK(len >= 4 && strcmp(ansi + len - 4, "\e[0m") == 0,
          "should end with reset, got: '%s'", ansi);
    ncc_free(ansi);
    PASS();
}

int
main(void)
{
    printf("=== ncc_string_to_ansi tests ===\n");

    test_plain_no_escapes();
    test_bold();
    test_italic();
    test_multiple_styles();
    test_uppercase();
    test_lowercase();
    test_null_input();
    test_bracket_syntax();
    test_unclosed_style();

    printf("\n%d tests, %d failures\n", test_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
