#pragma once

/**
 * @file grammar.h
 * @brief Grammar construction, match macros, and rule building.
 */

#include "parse/types.h"

// ============================================================================
// Grammar lifecycle
// ============================================================================

ncc_grammar_t *ncc_grammar_new(void);
void            ncc_grammar_free(ncc_grammar_t *g);
void            ncc_grammar_set_start_id(ncc_grammar_t *g, ncc_nt_id_t nt_id);

#define ncc_grammar_set_start(g, nt) \
    ncc_grammar_set_start_id((g), ncc_nonterm_id(nt))
void            ncc_grammar_set_error_recovery(ncc_grammar_t *g, bool enable);
void            ncc_grammar_set_max_penalty(ncc_grammar_t *g, uint32_t max);
void            ncc_grammar_finalize(ncc_grammar_t *g);

// ============================================================================
// Registration
// ============================================================================

ncc_nonterm_t *ncc_nonterm(ncc_grammar_t *g, ncc_string_t name);
int64_t         ncc_nonterm_id(ncc_nonterm_t *nt);
int64_t         ncc_register_terminal(ncc_grammar_t *g, ncc_string_t name);

// ============================================================================
// Walk actions / user data
// ============================================================================

void  ncc_nonterm_set_action(ncc_nonterm_t *nt, ncc_walk_action_t action);
void  ncc_nonterm_set_user_data(ncc_nonterm_t *nt, void *data);
void *ncc_nonterm_get_user_data(ncc_nonterm_t *nt);
void  ncc_grammar_set_default_action(ncc_grammar_t *g, ncc_walk_action_t a);
void  ncc_grammar_set_terminal_category(ncc_grammar_t *g, int64_t tid,
                                          ncc_string_t category);

// ============================================================================
// Match macros
// ============================================================================

#define NCC_NT(nt)       ((ncc_match_t){.kind = NCC_MATCH_NT, .nt_id = ncc_nonterm_id(nt)})
#define NCC_CHAR(cp)     ((ncc_match_t){.kind = NCC_MATCH_TERMINAL, .terminal_id = (int64_t)(cp)})
#define NCC_TERMINAL(id) ((ncc_match_t){.kind = NCC_MATCH_TERMINAL, .terminal_id = (id)})
#define NCC_ANY()        ((ncc_match_t){.kind = NCC_MATCH_ANY})
#define NCC_EPSILON()    ((ncc_match_t){.kind = NCC_MATCH_EMPTY})
#define NCC_CLASS(cc)    ((ncc_match_t){.kind = NCC_MATCH_CLASS, .char_class = (cc)})

// ============================================================================
// Group constructors (variadic via compound literal)
// ============================================================================

ncc_match_t ncc_group_match_v(ncc_grammar_t *g, int min, int max,
                                 int n, ncc_match_t *items);

#define ncc_optional(g, ...)                                                  \
    ncc_group_match_v((g), 0, 1,                                              \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})

#define ncc_star(g, ...)                                                      \
    ncc_group_match_v((g), 0, 0,                                              \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})

#define ncc_plus_group(g, ...)                                                \
    ncc_group_match_v((g), 1, 0,                                              \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})

#define ncc_repeat(g, min, max, ...)                                          \
    ncc_group_match_v((g), (min), (max),                                      \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})

// ============================================================================
// Rule building (variadic via compound literal)
// ============================================================================

ncc_parse_rule_t *ncc_add_rule_v(ncc_grammar_t *g, ncc_nt_id_t nt_id,
                                    int n, ncc_match_t *items);
ncc_parse_rule_t *ncc_add_rule_with_cost_v(ncc_grammar_t *g,
                                              ncc_nt_id_t nt_id,
                                              int cost,
                                              int n, ncc_match_t *items);

#define ncc_add_rule(g, nt, ...)                                              \
    ncc_add_rule_v((g), ncc_nonterm_id(nt),                                  \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})

#define ncc_add_rule_with_cost(g, nt, cost, ...)                              \
    ncc_add_rule_with_cost_v((g), ncc_nonterm_id(nt), (cost),                \
        sizeof((ncc_match_t[]){__VA_ARGS__}) / sizeof(ncc_match_t),           \
        (ncc_match_t[]){__VA_ARGS__})
