#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/c_tokenizer.h"
#include "parse/pwz.h"
#include "parse/symbol_populate.h"
#include "parse/type_infer.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "util/type_normalize.h"

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
    ncc_buffer_t *buf = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
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

int
main(void)
{
    const char *src =
        "[[n00b::comptime]] int answer = 42;\n"
        "int plain = 0;\n"
        "static int ncc_state_backing = 7;\n"
        "int *plain_addr = &ncc_state_backing;\n"
        "typedef struct n00b_buffer_t n00b_buffer_t;\n"
        "int *ncc_make(void);\n"
        "int *baked = ncc_make();\n"
        "unsigned long folded = sizeof(ncc_make());\n"
        "void *plain_image = nullptr;\n"
        "const n00b_buffer_t *buffer_image = __ncc_buflit(\"symtab\");\n"
        "int main(void) {\n"
        "    [[n00b::comptime]] int local = 1;\n"
        "    int *local_baked = ncc_make();\n"
        "    return answer + plain + local + (baked == local_baked)"
        " + (plain_addr != 0) + (folded != 0);\n"
        "}\n";

    ncc_grammar_t      *g = load_c_grammar();
    ncc_pwz_parser_t   *p = nullptr;
    ncc_token_stream_t *ts = nullptr;
    ncc_scanner_t      *scanner = nullptr;
    ncc_parse_tree_t   *tree = parse_source(g, src, &p, &ts, &scanner);

    ncc_symtab_t *st = ncc_populate_symbols(g, tree);
    ncc_sym_entry_t *answer = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("answer"));
    ncc_sym_entry_t *plain = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("plain"));
    ncc_sym_entry_t *plain_addr = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("plain_addr"));
    ncc_sym_entry_t *baked = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("baked"));
    ncc_sym_entry_t *folded = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("folded"));
    ncc_sym_entry_t *plain_image = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("plain_image"));
    ncc_sym_entry_t *buffer_image = ncc_symtab_lookup(
        st, ncc_string_empty(), NCC_STRING_STATIC("buffer_image"));

    assert(answer && answer->kind == NCC_SYM_VARIABLE);
    assert(answer->is_comptime);
    assert(!answer->is_static_init);
    char *answer_type = ncc_type_of_symbol(answer);
    assert(answer_type && strcmp(answer_type, "int") == 0);
    assert(ncc_type_hash_u64(answer_type) == ncc_type_hash_u64("int"));
    ncc_free(answer_type);
    assert(plain && plain->kind == NCC_SYM_VARIABLE);
    assert(!plain->is_comptime);
    assert(!plain->is_static_init);
    assert(plain_addr && plain_addr->kind == NCC_SYM_VARIABLE);
    assert(!plain_addr->is_comptime);
    assert(!plain_addr->is_static_init);
    // A general function-call initializer is NOT baked: the legacy
    // "bake any call-bearing static initializer" path was removed. Such a
    // declaration is left as ordinary C static storage (and the C compiler
    // rejects it if it isn't constant); compile-time init must use a
    // recognized literal or [[n00b::comptime]].
    assert(baked && baked->kind == NCC_SYM_VARIABLE);
    assert(!baked->is_comptime);
    assert(!baked->is_static_init);
    assert(!baked->static_init_needs_host_exec);
    assert(folded && folded->kind == NCC_SYM_VARIABLE);
    assert(!folded->is_comptime);
    assert(!folded->is_static_init);
    assert(plain_image && plain_image->kind == NCC_SYM_VARIABLE);
    assert(!plain_image->is_comptime);
    assert(!plain_image->is_static_init);
    assert(buffer_image && buffer_image->kind == NCC_SYM_VARIABLE);
    assert(!buffer_image->is_comptime);
    assert(buffer_image->is_static_init);
    assert(buffer_image->static_init_needs_host_exec);

    ncc_symtab_free(st);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_scanner_free(scanner);
    ncc_grammar_free(g);

    puts("PASS: comptime symtab flag");
    return 0;
}
