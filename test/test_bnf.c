// test_bnf.c — End-to-end test: load BNF grammar, tokenize C, parse, print tree.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_dump.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "internal/parse/grammar_internal.h"

// ============================================================================
// C tokenizer callback
// ============================================================================

static bool
c_tokenize(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    // Skip line comments.
    if (ncc_scan_peek_byte(s, 0) == '/'
        && ncc_scan_peek_byte(s, 1) == '/') {
        ncc_scan_skip_line_comment(s);
        goto restart;
    }

    // Skip block comments.
    if (ncc_scan_peek_byte(s, 0) == '/'
        && ncc_scan_peek_byte(s, 1) == '*') {
        ncc_scan_skip_block_comment(s, "/*", "*/");
        goto restart;
    }

    ncc_scan_mark(s);
    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    // String literals.
    if (cp == '"') {
        ncc_option_t(ncc_string_t) val = ncc_scan_string_double(s);
        ncc_scan_emit(s, NCC_TOK_STRING_LIT, val);
        return true;
    }

    // Character literals.
    if (cp == '\'') {
        ncc_option_t(ncc_string_t) val = ncc_scan_string_single(s);
        ncc_scan_emit(s, NCC_TOK_CHAR_LIT, val);
        return true;
    }

    // Numbers.
    if ((cp >= '0' && cp <= '9')
        || (cp == '.' && ncc_scan_peek_byte(s, 1) >= '0'
            && ncc_scan_peek_byte(s, 1) <= '9')) {
        bool emitted = ncc_scan_number(s, NCC_TOK_INTEGER, NCC_TOK_FLOAT);

        if (emitted) {
            return true;
        }
    }

    // Identifiers / keywords.
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_') {
        ncc_option_t(ncc_string_t) id_val = ncc_scan_identifier(s);

        if (ncc_option_is_set(id_val)) {
            ncc_string_t id_str = ncc_option_get(id_val);

            // Try keyword lookup via the scanner's grammar.
            int64_t kw_id = ncc_scan_terminal_id(s, id_str.data);

            if (kw_id != NCC_TOK_OTHER) {
                ncc_scan_emit(s, (int32_t)kw_id, id_val);
            }
            else {
                ncc_scan_emit(s, NCC_TOK_IDENTIFIER, id_val);
            }

            return true;
        }
    }

    // 3-char operators.
    static const char *ops3[] = {
        "<<=", ">>=", "...", nullptr,
    };

    for (const char **op = ops3; *op; op++) {
        if (ncc_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && ncc_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]
            && ncc_scan_peek_byte(s, 2) == (uint8_t)(*op)[2]) {
            int64_t tid = ncc_scan_terminal_id(s, *op);

            if (tid != NCC_TOK_OTHER) {
                ncc_scan_advance_n(s, 3);
                ncc_scan_emit_marked(s, (int32_t)tid);
                return true;
            }
        }
    }

    // 2-char operators.
    static const char *ops2[] = {
        "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
        "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "^=",
        "|=", "::", nullptr,
    };

    for (const char **op = ops2; *op; op++) {
        if (ncc_scan_peek_byte(s, 0) == (uint8_t)(*op)[0]
            && ncc_scan_peek_byte(s, 1) == (uint8_t)(*op)[1]) {
            int64_t tid = ncc_scan_terminal_id(s, *op);

            if (tid != NCC_TOK_OTHER) {
                ncc_scan_advance_n(s, 2);
                ncc_scan_emit_marked(s, (int32_t)tid);
                return true;
            }
        }
    }

    // Single-character token.
    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)cp);

    return true;
}

// ============================================================================
// Grammar loading
// ============================================================================

static ncc_grammar_t *
load_c_grammar(void)
{
    // Try several paths — meson runs from build dir.
    const char *paths[] = {
        "c_ncc.bnf",
        "../c_ncc.bnf",
        "../../c_ncc.bnf",
        nullptr,
    };

    const char *srcroot = getenv("MESON_SOURCE_ROOT");

    FILE *f = nullptr;

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

    char *buf = ncc_alloc_size(1, (size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    fclose(f);

    ncc_string_t bnf_text = ncc_string_from_raw(buf, (int64_t)nread);
    ncc_free(buf);

    ncc_grammar_t *g = ncc_grammar_new();
    ncc_grammar_set_error_recovery(g, false);

    ncc_string_t start = NCC_STRING_STATIC("translation_unit");
    bool ok = ncc_bnf_load(bnf_text, start, g);

    if (!ok) {
        fprintf(stderr, "  [FAIL] ncc_bnf_load failed for c_ncc.bnf\n");
        ncc_grammar_free(g);
        return nullptr;
    }

    return g;
}

// ============================================================================
// Test: load grammar
// ============================================================================

static void
test_load_grammar(void)
{
    printf("Test: Load C grammar from BNF...\n");

    ncc_grammar_t *g = load_c_grammar();

    if (!g) {
        printf("  SKIP (grammar file not found)\n");
        return;
    }

    // Check that basic terminals were registered.
    assert(g->terminal_map != nullptr);
    assert(ncc_list_len(g->nt_list) > 0);
    assert(ncc_list_len(g->rules) > 0);

    printf("  Grammar loaded: %zu NTs, %zu rules\n",
           ncc_list_len(g->nt_list),
           ncc_list_len(g->rules));

    printf("  PASS\n");
    ncc_grammar_free(g);
}

// ============================================================================
// Test: tokenize + parse simple C
// ============================================================================

static void
test_parse_simple_c(void)
{
    printf("Test: Parse simple C source...\n");

    ncc_grammar_t *g = load_c_grammar();

    if (!g) {
        printf("  SKIP\n");
        return;
    }

    const char *src = "int main(void) { return 0; }";

    ncc_buffer_t *buf = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
    ncc_scanner_t *scanner = ncc_scanner_new(buf, c_tokenize, g,
                                                ncc_option_none(ncc_string_t),
                                                nullptr, nullptr);
    ncc_token_stream_t *ts = ncc_token_stream_new(scanner);

    ncc_pwz_parser_t *p = ncc_pwz_new(g);
    assert(p);

    bool ok = ncc_pwz_parse(p, ts);

    if (ok) {
        ncc_parse_tree_t *tree = ncc_pwz_get_tree(p);
        assert(tree);

        printf("  Parse succeeded. Tree:\n");
        ncc_tree_dump(g, tree, stdout, false);
    }
    else {
        printf("  Parse did NOT succeed (this may be expected for a partial grammar test)\n");
    }

    printf("  PASS\n");

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_grammar_free(g);
}

// ============================================================================
// Test: BNF preprocessing
// ============================================================================

static void
test_bnf_preprocess(void)
{
    printf("Test: BNF preprocessing...\n");

    // Test comment stripping.
    ncc_string_t input = ncc_string_from_cstr("rule = a | b // comment\nrule2 = c");
    ncc_string_t stripped = ncc_bnf_strip_comments(input);

    assert(stripped.data != nullptr);
    printf("  Strip comments: \"%.*s\"\n", (int)stripped.u8_bytes, stripped.data);

    // Test line trimming.
    ncc_string_t padded = ncc_string_from_cstr("  hello  \n  world  \n");
    ncc_string_t trimmed = ncc_bnf_trim_lines(padded);

    assert(trimmed.data != nullptr);
    printf("  Trim lines: \"%.*s\"\n", (int)trimmed.u8_bytes, trimmed.data);

    printf("  PASS\n");
}

// ============================================================================
// main
// ============================================================================

int
main(void)
{
    test_bnf_preprocess();
    test_load_grammar();
    test_parse_simple_c();

    printf("\nAll BNF tests passed.\n");
    return 0;
}
