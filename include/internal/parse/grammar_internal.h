#pragma once

/**
 * @file grammar_internal.h
 * @internal
 * @brief Internal struct definitions for the ncc grammar system.
 */

#include "parse/grammar.h"
#include "lib/list.h"
#include "lib/dict.h"

// ============================================================================
// Container declarations
// ============================================================================

ncc_list_decl(ncc_match_t);
ncc_list_decl(ncc_terminal_t);
ncc_list_decl(ncc_parse_rule_t);
ncc_list_decl(ncc_nonterm_t);

// ============================================================================
// Internal structures
// ============================================================================

struct ncc_terminal_t {
    ncc_string_t value;
    void         *user_data;
    int64_t       id;
};

struct ncc_nonterm_t {
    ncc_string_t                    name;
    ncc_list_t(int32_t)             rule_ids;
    ncc_walk_action_t               action;
    void                            *user_data;
    ncc_dict_t             *first_set;
    int64_t                          id;
    bool                             group_nt;
    bool                             empty_is_error;
    bool                             error_nulled;
    bool                             start_nt;
    bool                             finalized;
    bool                             nullable;
    bool                             no_error_rule;
    bool                             first_has_any;
};

struct ncc_parse_rule_t {
    ncc_nt_id_t                          nt_id;
    ncc_list_t(ncc_match_t)             contents;
    ncc_dict_t                  *first_set;
    int32_t                               cost;
    int32_t                               link_ix;
    ncc_string_t                         doc;
    bool                                  penalty_rule;
    bool                                  first_has_any;
    void                                 *thunk;
};

struct ncc_rule_group_t {
    int64_t contents_id;
    int32_t min;
    int32_t max;
    int32_t gid;
};

// ============================================================================
// LR(0) types
// ============================================================================

typedef struct {
    int32_t rule_ix;
    int16_t dot;
    int16_t rule_len;
} ncc_lr0_item_t;

typedef struct {
    int32_t items_start;
    int32_t items_count;
    int32_t gotos_start;
    int32_t gotos_count;
} ncc_lr0_state_t;

typedef struct {
    int32_t symbol;
    int32_t dest_state;
} ncc_lr0_goto_t;

// ============================================================================
// Grammar struct
// ============================================================================

struct ncc_grammar_t {
    ncc_list_t(ncc_terminal_t)   named_terms;
    ncc_list_t(ncc_parse_rule_t) rules;
    ncc_list_t(ncc_nonterm_t)    nt_list;
    ncc_dict_t           *nt_map;
    ncc_dict_t           *terminal_map;
    int32_t                        default_start;
    uint32_t                       max_penalty;
    bool                           error_rules;
    bool                           finalized;
    bool                           has_groups;
    bool                           hide_penalty_rewrites;
    bool                           hide_groups;
    int                            suspend_penalty_hiding;
    bool                           suspend_group_hiding;
    ncc_walk_action_t             default_action;
    void                          *tokenize_cb;

    uint64_t *left_corner_sets;
    int32_t   lc_words_per_nt;

    ncc_lr0_item_t  *lr0_items;
    int32_t           lr0_item_count;
    int32_t          *lr0_rule_item_base;
    ncc_lr0_state_t *lr0_states;
    int32_t           lr0_state_count;
    int32_t          *lr0_state_items;
    int32_t           lr0_state_items_total;
    ncc_lr0_goto_t  *lr0_gotos;
    int32_t           lr0_goto_count;
    int32_t           lr0_start_state;
    int32_t          *lr0_predict_state;

    ncc_list_t(ncc_string_t) terminal_categories;
    bool                      has_terminal_categories;
};

// ============================================================================
// Inline helpers
// ============================================================================

static inline ncc_nonterm_t *
ncc_get_nonterm(ncc_grammar_t *g, int64_t id)
{
    if (id < 0 || (size_t)id >= ncc_list_len(g->nt_list)) {
        return nullptr;
    }
    return &g->nt_list.data[id];
}

static inline ncc_terminal_t *
ncc_get_terminal(ncc_grammar_t *g, int64_t id)
{
    int64_t ix = id - NCC_TOK_START_ID - 1;
    if (ix < 0 || (size_t)ix >= ncc_list_len(g->named_terms)) {
        return nullptr;
    }
    return &g->named_terms.data[ix];
}

static inline ncc_parse_rule_t *
ncc_get_rule(ncc_grammar_t *g, int32_t ix)
{
    if (ix < 0 || (size_t)ix >= ncc_list_len(g->rules)) {
        return nullptr;
    }
    return &g->rules.data[ix];
}

static inline bool
ncc_is_non_terminal(ncc_match_t *m)
{
    return m->kind == NCC_MATCH_NT || m->kind == NCC_MATCH_GROUP;
}

/**
 * @brief Look up the rule for a parse tree node.
 *
 * Parse tree nodes store a **local** rule index (position within the NT's
 * `rule_ids` list).  This helper converts to the global index and returns
 * the `ncc_parse_rule_t *`, or nullptr if anything is out of range.
 */
static inline ncc_parse_rule_t *
ncc_get_node_rule(ncc_grammar_t *g, ncc_nt_node_t *pn)
{
    ncc_nonterm_t *nt = ncc_get_nonterm(g, pn->id);

    if (!nt
            || pn->rule_index < 0
            || (size_t)pn->rule_index >= ncc_list_len(nt->rule_ids)) {
        return nullptr;
    }

    int32_t global_ix = ncc_list_get(nt->rule_ids, pn->rule_index);

    return ncc_get_rule(g, global_ix);
}

static inline bool
ncc_hide_groups(ncc_grammar_t *g)
{
    return g->hide_groups && !g->suspend_group_hiding;
}

static inline bool
ncc_hide_penalties(ncc_grammar_t *g)
{
    return g->hide_penalty_rewrites && !g->suspend_penalty_hiding;
}
