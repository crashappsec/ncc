#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/c_tokenizer.h"
#include "parse/comptime_check.h"
#include "parse/pwz.h"
#include "parse/symbol_populate.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "xform/transform.h"
#include "xform/xform_helpers.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    assert(f && "Cannot find c_ncc.bnf");

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
    assert(ncc_bnf_load(bnf_text, NCC_STRING_STATIC("translation_unit"), g));
    g->tokenize_cb = (void *)ncc_c_tokenize;
    return g;
}

static ncc_parse_tree_t *
parse_source(ncc_grammar_t *g, const char *src, ncc_pwz_parser_t **out_parser,
             ncc_token_stream_t **out_ts, ncc_scanner_t **out_scanner)
{
    ncc_buffer_t            *buf = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
    ncc_c_tokenizer_state_t *state = ncc_c_tokenizer_state_new();
    ncc_scanner_t *scanner = ncc_scanner_new(buf, ncc_c_tokenize, g,
                                             ncc_option_none(ncc_string_t),
                                             state, ncc_c_tokenizer_reset);
    ncc_token_stream_t *ts = ncc_token_stream_new(scanner);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    assert(ncc_pwz_parse(p, ts));
    ncc_parse_tree_t *tree = ncc_pwz_get_tree(p);
    assert(tree);

    *out_parser  = p;
    *out_ts      = ts;
    *out_scanner = scanner;
    return tree;
}

static int
check_source_optional(const char *src, bool expect_optional)
{
    ncc_grammar_t      *g = load_c_grammar();
    ncc_pwz_parser_t   *p = nullptr;
    ncc_token_stream_t *ts = nullptr;
    ncc_scanner_t      *scanner = nullptr;
    ncc_parse_tree_t   *tree = parse_source(g, src, &p, &ts, &scanner);

    ncc_xform_set_parent_pointers(tree);
    ncc_symtab_t *st = ncc_populate_symbols(g, tree);

    bool has_optional = !expect_optional;
    int  errors       = ncc_comptime_check(g, tree, st, &has_optional);
    assert(has_optional == expect_optional);
    if (expect_optional) {
        assert(!ncc_xform_subtree_carries_n00b_named_attr(tree, "optional"));
    }

    ncc_symtab_free(st);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_grammar_free(g);
    return errors;
}

static int
check_source(const char *src)
{
    return check_source_optional(src, false);
}

int
main(void)
{
    assert(check_source(
               "[[n00b::comptime]] int answer = 42;\n"
               "int comptime_main(int argc, char **argv, char **envp) {\n"
               "    answer = 1;\n"
               "    answer++;\n"
               "    ++answer;\n"
               "    (void)argc; (void)argv; (void)envp;\n"
               "    return 0;\n"
               "}\n"
               "int main(void) { return answer; }\n")
           == 0);

    assert(check_source(
               "[[n00b::comptime]] int answer;\n"
               "int main(void) {\n"
               "    answer = 1;\n"
               "    answer += 2;\n"
               "    answer++;\n"
               "    ++answer;\n"
               "    return answer;\n"
               "}\n")
           == 4);

    assert(check_source(
               "struct S { int x; };\n"
               "[[n00b::comptime]] struct S s;\n"
               "[[n00b::comptime]] int a[2];\n"
               "int main(void) {\n"
               "    s.x = 1;\n"
               "    a[0]++;\n"
               "    return 0;\n"
               "}\n")
           == 2);

    assert(check_source(
               "[[n00b::comptime]] int answer;\n"
               "int main(void) {\n"
               "    int answer = 0;\n"
               "    answer = 1;\n"
               "    answer++;\n"
               "    return answer;\n"
               "}\n")
           == 0);

    assert(check_source(
               "struct S { int x; };\n"
               "[[n00b::comptime]] struct S *root;\n"
               "int main(void) {\n"
               "    (*root).x = 1;\n"
               "    return 0;\n"
               "}\n")
           == 1);

    assert(check_source_optional(
               "[[n00b::optional]]\n"
               "int comptime_main(int argc, char **argv, char **envp) {\n"
               "    (void)argc; (void)argv; (void)envp;\n"
               "    return 0;\n"
               "}\n"
               "int main(void) { return 0; }\n",
               true)
           == 0);

    assert(check_source(
               "[[n00b::optional]] int main(void) { return 0; }\n")
           == 1);

    assert(check_source(
               "struct S { [[n00b::optional]] int x; };\n"
               "int main(void) { return 0; }\n")
           == 1);

    puts("PASS: comptime read-only/optional check");
    return 0;
}
