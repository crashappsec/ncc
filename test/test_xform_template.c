// test_xform_template.c — Unit tests for the xform template engine.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xform/xform_template.h"
#include "parse/bnf.h"
#include "parse/c_tokenizer.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"
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

// =========================================================================
// Grammar loading (same pattern as test_bnf.c)
// =========================================================================

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

    // Store the tokenizer callback on the grammar.
    g->tokenize_cb = (void *)ncc_c_tokenize;

    return g;
}

// =========================================================================
// Test 1: simple two-slot substitution
// =========================================================================
static void
test_simple_substitution(void)
{
    TEST("simple substitution ($0 + $1)");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    bool ok = ncc_template_register(&reg, "add",
                                     "additive_expression",
                                     "$0+$1");
    CHECK(ok, "register failed");

    const char *args[] = { "1", "2" };
    ncc_result_t(ncc_parse_tree_ptr_t) r
        = ncc_template_instantiate(&reg, "add", args, 2);

    CHECK(ncc_result_is_ok(r), "instantiate failed");

    ncc_parse_tree_t *tree = ncc_result_get(r);
    CHECK(tree != nullptr, "tree is nullptr");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Test 2: multi-slot substitution
// =========================================================================
static void
test_multi_slot(void)
{
    TEST("multi-slot substitution ($0, $1, $2)");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    bool ok = ncc_template_register(&reg, "ternary",
                                     "conditional_expression",
                                     "$0?$1:$2");
    CHECK(ok, "register failed");

    const char *args[] = { "x", "1", "0" };
    ncc_result_t(ncc_parse_tree_ptr_t) r
        = ncc_template_instantiate(&reg, "ternary", args, 3);

    CHECK(ncc_result_is_ok(r), "instantiate failed");

    ncc_parse_tree_t *tree = ncc_result_get(r);
    CHECK(tree != nullptr, "tree is nullptr");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Test 3: no-slot template (pure fixed text)
// =========================================================================
static void
test_no_slots(void)
{
    TEST("no slots (fixed text only)");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    bool ok = ncc_template_register(&reg, "fixed",
                                     "primary_expression",
                                     "42");
    CHECK(ok, "register failed");

    ncc_result_t(ncc_parse_tree_ptr_t) r
        = ncc_template_instantiate(&reg, "fixed", nullptr, 0);

    CHECK(ncc_result_is_ok(r), "instantiate failed");

    ncc_parse_tree_t *tree = ncc_result_get(r);
    CHECK(tree != nullptr, "tree is nullptr");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Test 4: not found error
// =========================================================================
static void
test_not_found(void)
{
    TEST("not found error");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    ncc_result_t(ncc_parse_tree_ptr_t) r
        = ncc_template_instantiate(&reg, "nonexistent", nullptr, 0);

    CHECK(ncc_result_is_err(r), "should have failed");
    CHECK(r.err == NCC_TMPL_ERR_NOT_FOUND, "wrong error code");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Test 5: reuse (same template, different args)
// =========================================================================
static void
test_reuse(void)
{
    TEST("template reuse with different args");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    bool ok = ncc_template_register(&reg, "add",
                                     "additive_expression",
                                     "$0+$1");
    CHECK(ok, "register failed");

    const char *args1[] = { "1", "2" };
    ncc_result_t(ncc_parse_tree_ptr_t) r1
        = ncc_template_instantiate(&reg, "add", args1, 2);
    CHECK(ncc_result_is_ok(r1), "first instantiate failed");

    const char *args2[] = { "x", "y" };
    ncc_result_t(ncc_parse_tree_ptr_t) r2
        = ncc_template_instantiate(&reg, "add", args2, 2);
    CHECK(ncc_result_is_ok(r2), "second instantiate failed");

    CHECK(ncc_result_get(r1) != nullptr, "tree1 is nullptr");
    CHECK(ncc_result_get(r2) != nullptr, "tree2 is nullptr");
    CHECK(ncc_result_get(r1) != ncc_result_get(r2), "trees are same pointer");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Test 6: rstr-shaped template (statement expression)
// =========================================================================
static void
test_rstr_shaped(void)
{
    TEST("rstr-shaped template (statement expression)");

    ncc_grammar_t *g = load_c_grammar();
    CHECK(g != nullptr, "grammar not loaded");

    ncc_template_registry_t reg;
    ncc_template_registry_init(&reg, g, ncc_c_tokenize);

    bool ok = ncc_template_register(&reg, "rstr_plain",
                                     "primary_expression",
                                     "({static ncc_string_t $0="
                                     "{.u8_bytes=$1,.data=$2,"
                                     ".codepoints=$3,.styling=((void*)0)}"
                                     ";&$0;})");
    CHECK(ok, "register failed");

    const char *args[] = {
        "_ncc_rs_0",      // $0 = var name
        "5",              // $1 = byte count
        "\"hello\"",      // $2 = data literal
        "5",              // $3 = codepoint count
    };
    ncc_result_t(ncc_parse_tree_ptr_t) r
        = ncc_template_instantiate(&reg, "rstr_plain", args, 4);

    CHECK(ncc_result_is_ok(r), "instantiate failed");

    ncc_parse_tree_t *tree = ncc_result_get(r);
    CHECK(tree != nullptr, "tree is nullptr");

    PASS();
    ncc_template_registry_free(&reg);
    ncc_grammar_free(g);
}

// =========================================================================
// Main
// =========================================================================

int
main(void)
{
    printf("=== xform_template tests ===\n");

    test_simple_substitution();
    test_multi_slot();
    test_no_slots();
    test_not_found();
    test_reuse();
    test_rstr_shaped();

    printf("\n%d tests, %d failures\n", test_count, fail_count);

    return fail_count ? 1 : 0;
}
