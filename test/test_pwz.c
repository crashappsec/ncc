// test_pwz.c — Smoke tests for the PWZ parser extraction.
//
// Tests:
//   1. Token-level parse: E → E "+" T | T ; T → ID
//   2. Codepoint-level parse: S → 'a' 'b'
//   3. Ambiguous grammar (forest): S → S S | 'a'

#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Terminal IDs.
#define TOK_PLUS (NCC_TOK_START_ID + 10)
#define TOK_ID   (NCC_TOK_START_ID + 11)

static void
test_basic_parse(void)
{
    ncc_grammar_t *g = ncc_grammar_new();

    int64_t E_id = ncc_nonterm_id(ncc_nonterm(g, ncc_string_from_cstr("E")));
    int64_t T_id = ncc_nonterm_id(ncc_nonterm(g, ncc_string_from_cstr("T")));

    ncc_register_terminal(g, ncc_string_from_cstr("PLUS"));
    ncc_register_terminal(g, ncc_string_from_cstr("ID"));

    // Use IDs via ncc_add_rule_v to avoid dangling pointers into nt_list
    // (ncc_add_rule creates error NTs which can reallocate nt_list).
    ncc_match_t rule1[] = {
        {.kind = NCC_MATCH_NT, .nt_id = E_id},
        NCC_TERMINAL(TOK_PLUS),
        {.kind = NCC_MATCH_NT, .nt_id = T_id},
    };
    ncc_add_rule_v(g, E_id, 3, rule1);

    ncc_match_t rule2[] = {{.kind = NCC_MATCH_NT, .nt_id = T_id}};
    ncc_add_rule_v(g, E_id, 1, rule2);

    ncc_match_t rule3[] = {NCC_TERMINAL(TOK_ID)};
    ncc_add_rule_v(g, T_id, 1, rule3);

    ncc_grammar_set_start_id(g, E_id);

    ncc_token_info_t tokens[3] = {
        { .tid = TOK_ID,   .index = 0, .line = 1, .column = 1,
          .value = ncc_option_set(ncc_string_t, ncc_string_from_cstr("a")) },
        { .tid = TOK_PLUS, .index = 1, .line = 1, .column = 3,
          .value = ncc_option_set(ncc_string_t, ncc_string_from_cstr("+")) },
        { .tid = TOK_ID,   .index = 2, .line = 1, .column = 5,
          .value = ncc_option_set(ncc_string_t, ncc_string_from_cstr("b")) },
    };

    ncc_token_info_t *ptrs[3] = { &tokens[0], &tokens[1], &tokens[2] };

    ncc_token_stream_t *ts = ncc_token_stream_from_array(ptrs, 3);
    assert(ts);

    ncc_pwz_parser_t *p = ncc_pwz_new(g);
    assert(p);

    bool ok = ncc_pwz_parse(p, ts);
    assert(ok && "parse should succeed");

    ncc_parse_tree_t *tree = ncc_pwz_get_tree(p);
    assert(tree && "should have a parse tree");

    assert(!ncc_tree_is_leaf(tree));
    ncc_nt_node_t *root = &ncc_tree_node_value(tree);
    assert(root->id == E_id);
    assert(ncc_tree_num_children(tree) == 3);

    void *result = ncc_parse_tree_walk(g, tree, nullptr);
    (void)result;

    printf("PASS: basic parse of 'a + b'\n");

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_grammar_free(g);
}

static void
test_codepoint_parse(void)
{
    ncc_grammar_t *g = ncc_grammar_new();

    ncc_nonterm_t *S = ncc_nonterm(g, ncc_string_from_cstr("S"));

    ncc_add_rule(g, S, NCC_CHAR('a'), NCC_CHAR('b'));

    ncc_grammar_set_start(g, S);

    ncc_string_t input = ncc_string_from_cstr("ab");
    ncc_token_stream_t *ts = ncc_token_stream_from_codepoints(input);
    assert(ts);

    ncc_pwz_parser_t *p = ncc_pwz_new(g);
    bool ok = ncc_pwz_parse(p, ts);
    assert(ok && "codepoint parse should succeed");

    ncc_parse_tree_t *tree = ncc_pwz_get_tree(p);
    assert(tree);

    printf("PASS: codepoint parse of 'ab'\n");

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_grammar_free(g);
}

static void
test_forest(void)
{
    ncc_grammar_t *g = ncc_grammar_new();

    ncc_nonterm_t *S = ncc_nonterm(g, ncc_string_from_cstr("S"));

    ncc_add_rule(g, S, NCC_NT(S), NCC_NT(S));
    ncc_add_rule(g, S, NCC_CHAR('a'));

    ncc_grammar_set_start(g, S);

    ncc_string_t input = ncc_string_from_cstr("aaa");
    ncc_token_stream_t *ts = ncc_token_stream_from_codepoints(input);

    ncc_parse_forest_t forest = ncc_pwz_parse_grammar(g, ts);

    int32_t count = ncc_parse_forest_count(&forest);
    assert(count >= 1 && "should have at least one parse");

    printf("PASS: ambiguous parse of 'aaa' (%d trees)\n", count);

    ncc_parse_forest_free(&forest);
    ncc_token_stream_free(ts);
    ncc_grammar_free(g);
}

int
main(void)
{
    test_basic_parse();
    test_codepoint_parse();
    test_forest();

    printf("\nAll PWZ smoke tests passed.\n");
    return 0;
}
