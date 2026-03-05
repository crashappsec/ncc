// grammar.c - Grammar definition, error rules, and finalization.
#include "parse/grammar.h"
#include "lib/alloc.h"
#include "internal/parse/grammar_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Nullability computation
// ============================================================================

static bool
check_stack_for_nt(ncc_list_t(int) *stack, int64_t ruleset)
{
    for (size_t i = 0; i < stack->len; i++) {
        if (ruleset == (int64_t)stack->data[i]) {
            return true;
        }
    }

    return false;
}

static inline void
finalize_nullable(ncc_nonterm_t *nt, bool value)
{
    nt->nullable  = value;
    nt->finalized = true;

    assert(!(nt->nullable && nt->group_nt) && "Group item cannot be nullable");
}

static bool
is_nullable_rule(ncc_grammar_t *g, ncc_parse_rule_t *rule, ncc_list_t(int) *stack);

static bool
is_nullable_nt(ncc_grammar_t *g, int64_t nt_id, ncc_list_t(int) *stack)
{
    if (stack && check_stack_for_nt(stack, nt_id)) {
        return true;
    }
    else {
        ncc_list_push(*stack, (int)nt_id);
    }

    ncc_nonterm_t *nt = ncc_get_nonterm(g, nt_id);

    if (nt->finalized) {
        return nt->nullable;
    }

    if (nt->rule_ids.len == 0) {
        finalize_nullable(nt, true);
        return true;
    }

    bool found_any_rule = false;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        ncc_parse_rule_t *cur_rule = ncc_get_rule(g, nt->rule_ids.data[i]);
        found_any_rule              = true;

        if (is_nullable_rule(g, cur_rule, stack)) {
            finalize_nullable(nt, true);
            (void)ncc_list_pop(int, *stack);
            return true;
        }
    }

    finalize_nullable(nt, !found_any_rule);
    (void)ncc_list_pop(int, *stack);

    return nt->nullable;
}

static bool
is_nullable_match(ncc_grammar_t *g, ncc_match_t *m, ncc_list_t(int) *stack)
{
    switch (m->kind) {
    case NCC_MATCH_EMPTY:
        return true;
    case NCC_MATCH_GROUP:
        return ((ncc_rule_group_t *)m->group)->min == 0;
    case NCC_MATCH_NT:
        return is_nullable_nt(g, m->nt_id, stack);
    default:
        return false;
    }
}

static bool
is_nullable_rule(ncc_grammar_t *g, ncc_parse_rule_t *rule, ncc_list_t(int) *stack)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *item = &rule->contents.data[i];

        if (!is_nullable_match(g, item, stack)) {
            return false;
        }

        if (item->kind == NCC_MATCH_NT) {
            if (check_stack_for_nt(stack, item->nt_id)) {
                return false;
            }

            return is_nullable_nt(g, item->nt_id, stack);
        }
    }

    return false;
}

// ============================================================================
// Internal: rule management
// ============================================================================

static bool
matches_eq(ncc_match_t *a, ncc_match_t *b)
{
    if (a->kind != b->kind) {
        return false;
    }

    switch (a->kind) {
    case NCC_MATCH_EMPTY:
    case NCC_MATCH_ANY:
        return true;
    case NCC_MATCH_TERMINAL:
        return a->terminal_id == b->terminal_id;
    case NCC_MATCH_CLASS:
        return a->char_class == b->char_class;
    case NCC_MATCH_NT:
        return a->nt_id == b->nt_id;
    case NCC_MATCH_GROUP:
        return a->group == b->group;
    case NCC_MATCH_SET:
        return a->set_items == b->set_items;
    }

    return false;
}

static bool
rule_exists(ncc_grammar_t *g,
            ncc_nonterm_t *nt,
            ncc_list_t(ncc_match_t) *new_contents,
            int32_t *old_ix_out)
{
    size_t new_len = new_contents->len;

    for (size_t i = 0; i < nt->rule_ids.len; i++) {
        int32_t            rule_ix = nt->rule_ids.data[i];
        ncc_parse_rule_t *old     = ncc_get_rule(g, rule_ix);

        if (old->contents.len != new_len) {
            continue;
        }

        bool match = true;

        for (size_t j = 0; j < new_len; j++) {
            if (!matches_eq(&old->contents.data[j], &new_contents->data[j])) {
                match = false;
                break;
            }
        }

        if (match) {
            if (old_ix_out) {
                *old_ix_out = rule_ix;
            }

            return true;
        }
    }

    return false;
}

static int32_t
add_rule_internal(ncc_grammar_t *g,
                  ncc_nt_id_t    nt_id,
                  ncc_list_t(ncc_match_t) *items,
                  int      cost,
                  int32_t  penalty_link_ix,
                  int32_t *old_ix_out)
{
    assert(!g->finalized && "Cannot modify grammar after finalization");
    assert(items->len > 0 && "Empty productions not allowed");

    ncc_nonterm_t *nt = ncc_get_nonterm(g, nt_id);

    if (rule_exists(g, nt, items, old_ix_out)) {
        return -1;
    }

    ncc_parse_rule_t rule = {0};

    rule.nt_id        = nt_id;
    rule.contents     = *items;
    rule.cost         = cost;
    rule.penalty_rule = penalty_link_ix >= 0;
    rule.link_ix      = penalty_link_ix;

    ncc_list_push(g->rules, rule);

    int32_t ix = (int32_t)(g->rules.len - 1);
    ncc_list_push(nt->rule_ids, ix);

    return ix;
}

// ============================================================================
// Error rule generation
// ============================================================================

static void
create_one_error_rule_set(ncc_grammar_t *g, int32_t rule_ix)
{
    ncc_parse_rule_t *cur = ncc_get_rule(g, rule_ix);
    ncc_nt_id_t cur_nt_id = cur->nt_id;
    ncc_nonterm_t *cur_nt = ncc_get_nonterm(g, cur_nt_id);

    if (cur_nt->no_error_rule) {
        return;
    }

    size_t n      = cur->contents.len;
    int    tok_ct = 0;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *m = &cur->contents.data[i];

        if (m->kind == NCC_MATCH_NT || m->kind == NCC_MATCH_GROUP) {
            continue;
        }

        tok_ct++;

        // Re-resolve the nt pointer since a previous iteration's
        // ncc_nonterm() may have reallocated nt_list.
        ncc_nonterm_t *cur_nt = ncc_get_nonterm(g, cur_nt_id);

        char namebuf[256];
        snprintf(namebuf,
                 sizeof(namebuf),
                 "$term-%s-%d-%d",
                 cur_nt->name.data ? cur_nt->name.data : "?",
                 rule_ix,
                 tok_ct);
        ncc_string_t name = ncc_string_from_cstr(namebuf);

        ncc_nonterm_t *nt_err    = ncc_nonterm(g, name);
        ncc_nt_id_t    nt_err_id = ncc_nonterm_id(nt_err);

        // Rule 1: match the terminal normally (cost 0)
        ncc_list_t(ncc_match_t) r1 = ncc_list_new(ncc_match_t);
        ncc_list_push(r1, *m);
        int32_t ok_ix = add_rule_internal(g, nt_err_id, &r1, 0, -1, nullptr);

        // Rule 2: omit the terminal (penalty = token omission)
        ncc_list_t(ncc_match_t) r2 = ncc_list_new(ncc_match_t);
        ncc_match_t empty           = {.kind = NCC_MATCH_EMPTY};
        ncc_list_push(r2, empty);
        add_rule_internal(g, nt_err_id, &r2, 0, ok_ix, nullptr);

        // Rule 3: junk prefix + terminal (bad prefix penalty)
        ncc_list_t(ncc_match_t) r3 = ncc_list_new(ncc_match_t);
        ncc_match_t any             = {.kind = NCC_MATCH_ANY};
        ncc_list_push(r3, any);
        ncc_list_push(r3, *m);
        add_rule_internal(g, nt_err_id, &r3, 0, ok_ix, nullptr);

        // Re-fetch cur since the rule list may have been reallocated.
        cur                   = ncc_get_rule(g, rule_ix);
        cur->contents.data[i] = (ncc_match_t){.kind = NCC_MATCH_NT, .nt_id = nt_err_id};
    }
}

// ============================================================================
// Public API
// ============================================================================

ncc_grammar_t *
ncc_grammar_new(void)
{
    ncc_grammar_t *g = ncc_alloc(ncc_grammar_t);

    g->error_rules           = true;
    g->max_penalty           = NCC_DEFAULT_MAX_PENALTY;
    g->hide_penalty_rewrites = true;
    g->hide_groups           = true;

    g->nt_map = ncc_alloc(ncc_dict_t);
    ncc_dict_init(g->nt_map, ncc_hash_cstring, ncc_dict_cstr_eq);

    g->terminal_map = ncc_alloc(ncc_dict_t);
    ncc_dict_init(g->terminal_map, ncc_hash_cstring, ncc_dict_cstr_eq);

    return g;
}

void
ncc_grammar_free(ncc_grammar_t *g)
{
    if (!g) {
        return;
    }

    for (size_t i = 0; i < g->rules.len; i++) {
        ncc_list_free(g->rules.data[i].contents);
    }

    ncc_list_free(g->rules);

    for (size_t i = 0; i < g->nt_list.len; i++) {
        ncc_nonterm_t *nt = &g->nt_list.data[i];

        ncc_list_free(nt->rule_ids);

        // Annotation structs and lists are GC-managed; no manual free needed.
    }

    ncc_list_free(g->nt_list);
    ncc_list_free(g->named_terms);

    // The untyped dicts are GC-managed; explicit free for predictable cleanup.
    if (g->nt_map) {
        ncc_free(g->nt_map);
    }
    if (g->terminal_map) {
        ncc_free(g->terminal_map);
    }

    if (g->left_corner_sets) {
        ncc_free(g->left_corner_sets);
    }

    if (g->lr0_items) {
        ncc_free(g->lr0_items);
    }
    if (g->lr0_rule_item_base) {
        ncc_free(g->lr0_rule_item_base);
    }
    if (g->lr0_states) {
        ncc_free(g->lr0_states);
    }
    if (g->lr0_state_items) {
        ncc_free(g->lr0_state_items);
    }
    if (g->lr0_gotos) {
        ncc_free(g->lr0_gotos);
    }
    if (g->lr0_predict_state) {
        ncc_free(g->lr0_predict_state);
    }

    if (g->has_terminal_categories) {
        for (size_t i = 0; i < g->terminal_categories.len; i++) {
            ncc_free(g->terminal_categories.data[i].data);
        }
        ncc_list_free(g->terminal_categories);
    }

    ncc_free(g);
}

void
ncc_grammar_set_start_id(ncc_grammar_t *g, ncc_nt_id_t nt_id)
{
    g->default_start = nt_id;
}

void
ncc_grammar_set_error_recovery(ncc_grammar_t *g, bool enable)
{
    g->error_rules = enable;
}

void
ncc_grammar_set_max_penalty(ncc_grammar_t *g, uint32_t max)
{
    g->max_penalty = max;
}

ncc_nonterm_t *
ncc_nonterm(ncc_grammar_t *g, ncc_string_t name)
{
    if (name.data) {
        bool  found = false;
        void *val   = _ncc_dict_get(g->nt_map, (void *)name.data, &found);

        if (found) {
            int64_t existing = (int64_t)(intptr_t)val;
            return ncc_get_nonterm(g, existing);
        }
    }

    ncc_nonterm_t nt = {0};

    nt.name = name;
    nt.id   = (int64_t)g->nt_list.len;

    ncc_list_push(g->nt_list, nt);

    if (name.data) {
        ncc_nonterm_t *stored = ncc_get_nonterm(g, nt.id);
        _ncc_dict_put(g->nt_map,
                               (void *)stored->name.data,
                               (void *)(intptr_t)stored->id);
    }

    return ncc_get_nonterm(g, nt.id);
}

int64_t
ncc_nonterm_id(ncc_nonterm_t *nt)
{
    return nt->id;
}

int64_t
ncc_register_terminal(ncc_grammar_t *g, ncc_string_t name)
{
    bool  found = false;
    void *val   = _ncc_dict_get(g->terminal_map, (void *)name.data, &found);

    if (found) {
        return (int64_t)(intptr_t)val;
    }

    if (name.u8_bytes == 1) {
        return (int64_t)(unsigned char)name.data[0];
    }

    ncc_terminal_t term = {0};

    term.value = name;
    term.id    = (int64_t)g->named_terms.len + NCC_TOK_START_ID + 1;

    ncc_list_push(g->named_terms, term);

    ncc_terminal_t *stored = ncc_get_terminal(g, term.id);
    _ncc_dict_put(g->terminal_map,
                           (void *)stored->value.data,
                           (void *)(intptr_t)stored->id);

    return term.id;
}

void
ncc_grammar_set_terminal_category(ncc_grammar_t *g,
                                   int64_t         terminal_id,
                                   ncc_string_t   category)
{
    if (terminal_id < NCC_TOK_START_ID) {
        return; // single-char terminals self-describe
    }

    int32_t ix = (int32_t)(terminal_id - NCC_TOK_START_ID);

    if (!g->has_terminal_categories) {
        g->terminal_categories     = ncc_list_new(ncc_string_t);
        g->has_terminal_categories = true;
    }

    // Extend list with empty strings up to the required index.
    while ((int32_t)g->terminal_categories.len <= ix) {
        ncc_list_push(g->terminal_categories, (ncc_string_t){0});
    }

    ncc_list_set(g->terminal_categories, ix, category);
}

void
ncc_set_action(ncc_nonterm_t *nt, ncc_walk_action_t action)
{
    nt->action = action;
}

void
ncc_nonterm_set_user_data(ncc_nonterm_t *nt, void *data)
{
    nt->user_data = data;
}

void *
ncc_nonterm_get_user_data(ncc_nonterm_t *nt)
{
    return nt->user_data;
}

void
ncc_nonterm_set_action(ncc_nonterm_t *nt, ncc_walk_action_t action)
{
    if (nt) {
        nt->action = action;
    }
}

void
ncc_grammar_set_default_action(ncc_grammar_t *g, ncc_walk_action_t action)
{
    if (g) {
        g->default_action = action;
    }
}

// ============================================================================
// Group match
// ============================================================================

static int32_t group_id_counter = 1;

ncc_match_t
ncc_group_match_v(ncc_grammar_t *g, int min, int max, int n, ncc_match_t *items)
{
    ncc_rule_group_t *group = ncc_alloc(ncc_rule_group_t);

    group->gid = group_id_counter++;
    group->min = min;
    group->max = max;

    char namebuf[64];
    snprintf(namebuf, sizeof(namebuf), "$$group_%d", group->gid);
    ncc_string_t name = ncc_string_from_cstr(namebuf);

    ncc_nonterm_t *nt = ncc_nonterm(g, name);
    int64_t nt_id = ncc_nonterm_id(nt);

    nt->group_nt = true;

    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    add_rule_internal(g, nt_id, &match_items, 0, -1, nullptr);

    group->contents_id = nt_id;

    return (ncc_match_t){.kind = NCC_MATCH_GROUP, .group = group};
}

// ============================================================================
// Rule building
// ============================================================================

ncc_parse_rule_t *
ncc_add_rule_v(ncc_grammar_t *g, ncc_nt_id_t nt_id, int n, ncc_match_t *items)
{
    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, 0, -1, &old_ix);

    return ncc_get_rule(g, ix >= 0 ? ix : old_ix);
}

ncc_parse_rule_t *
ncc_add_rule_with_cost_v(ncc_grammar_t *g,
                          ncc_nt_id_t    nt_id,
                          int             cost,
                          int             n,
                          ncc_match_t   *items)
{
    ncc_list_t(ncc_match_t) match_items = ncc_list_new(ncc_match_t);

    for (int i = 0; i < n; i++) {
        ncc_list_push(match_items, items[i]);
    }

    int32_t old_ix = -1;
    int32_t ix     = add_rule_internal(g, nt_id, &match_items, cost, -1, &old_ix);

    return ncc_get_rule(g, ix >= 0 ? ix : old_ix);
}

// ============================================================================
// FIRST set computation (iterative fixed-point)
// ============================================================================

// Encode a terminal ID as a void* for use in dict-as-set FIRST sets.
// Offset by 0x100 to avoid nullptr (0) collisions.
#define TERM_TO_PTR(id) ((void *)(uintptr_t)((uint64_t)(id) + 0x100))

static inline ncc_dict_t *
ncc_dict_set_new(void)
{
    ncc_dict_t *d = ncc_alloc(ncc_dict_t);
    ncc_dict_init(d, nullptr, nullptr);
    return d;
}

static inline void
merge_dict_set_into(ncc_dict_t *dst, ncc_dict_t *src)
{
    if (!src) {
        return;
    }

    for (size_t i = 0; i < src->capacity; i++) {
        if (src->buckets[i].state == _NCC_BUCKET_OCCUPIED) {
            ncc_dict_add(dst, src->buckets[i].key, (void *)1);
        }
    }
}

// Check if a match can derive the empty string, using the first_nullable
// array instead of nt->nullable (which has a known bug for non-NT-ending rules).
static inline bool
match_is_first_nullable(ncc_grammar_t *g, ncc_match_t *m, bool *first_nullable)
{
    (void)g;
    switch (m->kind) {
    case NCC_MATCH_EMPTY:
        return true;
    case NCC_MATCH_GROUP:
        return ((ncc_rule_group_t *)m->group)->min == 0;
    case NCC_MATCH_NT:
        return first_nullable[m->nt_id];
    default:
        return false;
    }
}

// Check if a rule can derive the empty string.
static bool
rule_is_first_nullable(ncc_grammar_t *g, ncc_parse_rule_t *rule, bool *fn)
{
    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        if (!match_is_first_nullable(g, &rule->contents.data[i], fn)) {
            return false;
        }
    }

    return n > 0;
}

// Compute the FIRST set for a single rule, given current NT FIRST sets.
// Returns true if the rule's FIRST set changed.
static bool
update_rule_first(ncc_grammar_t *g, ncc_parse_rule_t *rule, bool *first_nullable)
{
    if (!rule->first_set) {
        rule->first_set = ncc_dict_set_new();
    }

    size_t old_len      = rule->first_set->count;
    bool    old_has_any = rule->first_has_any;

    size_t n = rule->contents.len;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *m = &rule->contents.data[i];

        switch (m->kind) {
        case NCC_MATCH_TERMINAL:
            ncc_dict_add(rule->first_set, TERM_TO_PTR(m->terminal_id), (void *)1);
            goto done;
        case NCC_MATCH_ANY:
        case NCC_MATCH_CLASS:
        case NCC_MATCH_SET:
            rule->first_has_any = true;
            goto done;
        case NCC_MATCH_EMPTY:
            break;
        case NCC_MATCH_GROUP: {
            ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;
            ncc_nonterm_t    *gnt = ncc_get_nonterm(g, grp->contents_id);

            if (gnt) {
                if (gnt->first_set) {
                    merge_dict_set_into(rule->first_set, gnt->first_set);
                }

                if (gnt->first_has_any) {
                    rule->first_has_any = true;
                }
            }

            if (grp->min > 0) {
                goto done;
            }

            break;
        }
        case NCC_MATCH_NT: {
            ncc_nonterm_t *nt = ncc_get_nonterm(g, m->nt_id);

            if (nt) {
                if (nt->first_set) {
                    merge_dict_set_into(rule->first_set, nt->first_set);
                }

                if (nt->first_has_any) {
                    rule->first_has_any = true;
                }

                if (!first_nullable[m->nt_id]) {
                    goto done;
                }
            }
            else {
                rule->first_has_any = true;
                goto done;
            }

            break;
        }
        }
    }

done:
    return rule->first_set->count != old_len || rule->first_has_any != old_has_any;
}

static void
compute_all_first_sets(ncc_grammar_t *g)
{
    size_t n_nts   = g->nt_list.len;
    size_t n_rules = g->rules.len;

    // Compute correct nullability for FIRST set purposes.
    // The parser's nt->nullable has a known limitation for rules ending
    // with non-NT nullable items (EMPTY, groups with min==0).  We compute
    // a separate array here via iterative fixed point.
    bool *first_nullable = ncc_alloc_array(bool, n_nts);

    bool fn_changed = true;

    while (fn_changed) {
        fn_changed = false;

        for (size_t i = 0; i < n_nts; i++) {
            if (first_nullable[i]) {
                continue;
            }

            ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)i);

            for (size_t j = 0; j < nt->rule_ids.len; j++) {
                ncc_parse_rule_t *rule = ncc_get_rule(g, nt->rule_ids.data[j]);

                if (rule_is_first_nullable(g, rule, first_nullable)) {
                    first_nullable[i] = true;
                    fn_changed        = true;
                    break;
                }
            }
        }
    }

    // Initialize all FIRST sets.
    for (size_t i = 0; i < n_nts; i++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)i);

        nt->first_set     = ncc_dict_set_new();
        nt->first_has_any = false;
    }

    for (size_t i = 0; i < n_rules; i++) {
        ncc_parse_rule_t *rule = ncc_get_rule(g, (int32_t)i);

        rule->first_set     = ncc_dict_set_new();
        rule->first_has_any = false;
    }

    // Iterate until fixed point.
    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 0; i < n_rules; i++) {
            ncc_parse_rule_t *rule = ncc_get_rule(g, (int32_t)i);

            if (update_rule_first(g, rule, first_nullable)) {
                changed = true;
            }
        }

        // Merge rule FIRST sets into their NTs.
        for (size_t i = 0; i < n_nts; i++) {
            ncc_nonterm_t *nt      = ncc_get_nonterm(g, (int64_t)i);
            size_t          old_len = nt->first_set->count;
            bool            old_any = nt->first_has_any;

            for (size_t j = 0; j < nt->rule_ids.len; j++) {
                int32_t            rule_ix = nt->rule_ids.data[j];
                ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);

                merge_dict_set_into(nt->first_set, rule->first_set);

                if (rule->first_has_any) {
                    nt->first_has_any = true;
                }
            }

            if (nt->first_set->count != old_len || nt->first_has_any != old_any) {
                changed = true;
            }
        }
    }

    // Nullable NTs must not be filtered by FIRST: if the NT derives empty,
    // any token is compatible (it matches whatever follows the NT).
    for (size_t i = 0; i < n_nts; i++) {
        if (first_nullable[i]) {
            ncc_get_nonterm(g, (int64_t)i)->first_has_any = true;
        }
    }

    ncc_free(first_nullable);
}

// ============================================================================
// Left-corner sets (flat bitset array in grammar)
// ============================================================================

static void
compute_left_corners(ncc_grammar_t *g)
{
    size_t n_nts = g->nt_list.len;

    if (n_nts == 0) {
        return;
    }

    int32_t words = (int32_t)((n_nts + 63) / 64);

    g->lc_words_per_nt  = words;
    g->left_corner_sets = ncc_alloc_array(uint64_t, n_nts * (size_t)words);

    // Each NT includes itself.
    for (size_t i = 0; i < n_nts; i++) {
        uint64_t *set = g->left_corner_sets + i * (size_t)words;

        set[i / 64] |= (uint64_t)1 << (i % 64);
    }

    // Compute direct left-corners.
    for (size_t i = 0; i < n_nts; i++) {
        ncc_nonterm_t *nt  = ncc_get_nonterm(g, (int64_t)i);
        uint64_t       *set = g->left_corner_sets + i * (size_t)words;

        for (size_t j = 0; j < nt->rule_ids.len; j++) {
            int32_t            rule_ix = nt->rule_ids.data[j];
            ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);
            size_t             n_match = rule->contents.len;

            for (size_t k = 0; k < n_match; k++) {
                ncc_match_t *m = &rule->contents.data[k];

                if (m->kind == NCC_MATCH_NT) {
                    size_t child_id = (size_t)m->nt_id;

                    set[child_id / 64] |= (uint64_t)1 << (child_id % 64);

                    ncc_nonterm_t *child = ncc_get_nonterm(g, m->nt_id);

                    if (!child->nullable) {
                        break;
                    }
                }
                else if (m->kind == NCC_MATCH_GROUP) {
                    ncc_rule_group_t *grp      = (ncc_rule_group_t *)m->group;
                    size_t             child_id = (size_t)grp->contents_id;

                    set[child_id / 64] |= (uint64_t)1 << (child_id % 64);

                    if (grp->min > 0) {
                        ncc_nonterm_t *gnt = ncc_get_nonterm(g, grp->contents_id);

                        if (!gnt->nullable) {
                            break;
                        }
                    }
                }
                else if (m->kind == NCC_MATCH_EMPTY) {
                    continue;
                }
                else {
                    break;
                }
            }
        }
    }

    // Transitive closure.
    bool changed = true;

    while (changed) {
        changed = false;

        for (size_t i = 0; i < n_nts; i++) {
            uint64_t *set_i = g->left_corner_sets + i * (size_t)words;

            for (size_t j = 0; j < n_nts; j++) {
                if (!((set_i[j / 64] >> (j % 64)) & 1)) {
                    continue;
                }

                uint64_t *set_j = g->left_corner_sets + j * (size_t)words;

                for (int32_t w = 0; w < words; w++) {
                    uint64_t new_bits = set_j[w] & ~set_i[w];

                    if (new_bits) {
                        set_i[w] |= new_bits;
                        changed = true;
                    }
                }
            }
        }
    }
}

// ============================================================================
// LR(0) state table construction
// ============================================================================

// Encode a ncc_match_t as an int32_t symbol for GOTO keys.
// NT -> nt_id, TERMINAL -> terminal_id (offset to avoid overlap with NTs),
// GROUP -> -(1000+gid), ANY -> -1, EMPTY -> -2, CLASS -> -(100+class_id)
static int32_t
lr0_symbol_of_match(ncc_match_t *m)
{
    switch (m->kind) {
    case NCC_MATCH_NT:
        return (int32_t)m->nt_id;
    case NCC_MATCH_TERMINAL:
        return (int32_t)m->terminal_id + 0x10000;
    case NCC_MATCH_GROUP: {
        ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;
        return -(1000 + grp->gid);
    }
    case NCC_MATCH_ANY:
        return -1;
    case NCC_MATCH_EMPTY:
        return -2;
    case NCC_MATCH_CLASS:
        return -(100 + (int32_t)m->char_class);
    case NCC_MATCH_SET:
        return -3;
    }

    return -999;
}

// Build lr0_items[] and lr0_rule_item_base[].
// For each rule, items at dot 0..len.
static void
compute_lr0_items(ncc_grammar_t *g)
{
    size_t n_rules = g->rules.len;

    g->lr0_rule_item_base = ncc_alloc_array(int32_t, n_rules);

    // Count total items first.
    int32_t total = 0;

    for (size_t i = 0; i < n_rules; i++) {
        ncc_parse_rule_t *r   = ncc_get_rule(g, (int32_t)i);
        int32_t            len = (int32_t)r->contents.len;

        g->lr0_rule_item_base[i] = total;
        total += len + 1; // dot positions 0..len
    }

    g->lr0_items      = ncc_alloc_array(ncc_lr0_item_t, total);
    g->lr0_item_count = total;

    for (size_t i = 0; i < n_rules; i++) {
        ncc_parse_rule_t *r    = ncc_get_rule(g, (int32_t)i);
        int32_t            len  = (int32_t)r->contents.len;
        int32_t            base = g->lr0_rule_item_base[i];

        for (int32_t d = 0; d <= len; d++) {
            g->lr0_items[base + d] = (ncc_lr0_item_t){
                .rule_ix  = (int32_t)i,
                .dot      = (int16_t)d,
                .rule_len = (int16_t)len,
            };
        }
    }
}

// Compute the LR(0) closure of a set of items.
// seed_bits is a bitset over lr0_items; seed_list/seed_len are the explicit
// list of item_ids in the seed. On return, seed_bits and seed_list/seed_len
// are the closed set.
static void
lr0_closure(ncc_grammar_t *g, uint64_t *seed_bits, int32_t *seed_list, int32_t *seed_len)
{
    int32_t words = (g->lr0_item_count + 63) / 64;
    (void)words;

    // Fixed-point: process items in seed_list; if dot is before NT or GROUP,
    // add that NT's rules at dot=0.
    for (int32_t idx = 0; idx < *seed_len; idx++) {
        int32_t          item_id = seed_list[idx];
        ncc_lr0_item_t *item    = &g->lr0_items[item_id];

        if (item->dot >= item->rule_len) {
            continue;
        }

        ncc_parse_rule_t *rule = ncc_get_rule(g, item->rule_ix);
        ncc_match_t      *m    = &rule->contents.data[item->dot];

        int64_t predict_nt_id = -1;

        if (m->kind == NCC_MATCH_NT) {
            predict_nt_id = m->nt_id;
        }
        else if (m->kind == NCC_MATCH_GROUP) {
            ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;
            predict_nt_id          = grp->contents_id;
        }

        if (predict_nt_id < 0) {
            continue;
        }

        ncc_nonterm_t *nt = ncc_get_nonterm(g, predict_nt_id);

        if (!nt) {
            continue;
        }

        for (size_t r = 0; r < nt->rule_ids.len; r++) {
            int32_t rix      = nt->rule_ids.data[r];
            int32_t new_item = g->lr0_rule_item_base[rix]; // dot=0

            size_t   w    = (size_t)new_item / 64;
            uint64_t mask = (uint64_t)1 << ((size_t)new_item % 64);

            if (!(seed_bits[w] & mask)) {
                seed_bits[w] |= mask;
                seed_list[(*seed_len)++] = new_item;
            }
        }
    }
}

// Hash an item set (sorted array of item_ids) for state deduplication.
static uint64_t
lr0_state_hash(int32_t *items, int32_t count)
{
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (uint64_t)count;

    for (int32_t i = 0; i < count; i++) {
        h ^= ((uint64_t)items[i] + 0x9e3779b97f4a7c15ULL);
        h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
        h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
        h ^= h >> 31;
    }

    return h;
}

// Simple comparison for sorted item arrays.
static bool
lr0_items_eq(int32_t *a, int32_t a_len, int32_t *b, int32_t b_len)
{
    if (a_len != b_len) {
        return false;
    }

    return memcmp(a, b, (size_t)a_len * sizeof(int32_t)) == 0;
}

// qsort comparator for int32_t.
static int
cmp_int32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;

    return (x > y) - (x < y);
}

// Builder context for LR(0) state construction.
typedef struct {
    ncc_grammar_t  *g;
    int32_t         *state_starts;
    int32_t         *state_counts;
    int32_t         *state_items;
    int32_t          state_items_total;
    int32_t          state_items_cap;
    int32_t         *goto_starts;
    int32_t         *goto_counts;
    ncc_lr0_goto_t *gotos;
    int32_t          gotos_cap;
    int32_t          gotos_count;
    int32_t          states_cap;
    int32_t          states_count;
    int64_t         *dedup_hash;
    int32_t         *dedup_sid;
    int32_t          dedup_cap;
} lr0_builder_t;

// Find or create a state from a closed, sorted item set. Returns state_id.
static int32_t
lr0_find_or_create(lr0_builder_t *b, int32_t *items_buf, int32_t count)
{
    uint64_t h    = lr0_state_hash(items_buf, count);
    int32_t  mask = b->dedup_cap - 1;
    int32_t  idx  = (int32_t)(h & (uint64_t)mask);

    while (b->dedup_hash[idx]) {
        if ((uint64_t)b->dedup_hash[idx] == h) {
            int32_t sid   = b->dedup_sid[idx];
            int32_t s_off = b->state_starts[sid];
            int32_t s_cnt = b->state_counts[sid];

            if (lr0_items_eq(items_buf, count, &b->state_items[s_off], s_cnt)) {
                return sid;
            }
        }

        idx = (idx + 1) & mask;
    }

    // New state.
    int32_t sid = b->states_count++;

    if (sid >= b->states_cap) {
        int32_t old_cap = b->states_cap;
        b->states_cap *= 2;

        int32_t *new_state_starts = ncc_alloc_array(int32_t, b->states_cap);
        memcpy(new_state_starts, b->state_starts, (size_t)old_cap * sizeof(int32_t));
        ncc_free(b->state_starts);
        b->state_starts = new_state_starts;

        int32_t *new_state_counts = ncc_alloc_array(int32_t, b->states_cap);
        memcpy(new_state_counts, b->state_counts, (size_t)old_cap * sizeof(int32_t));
        ncc_free(b->state_counts);
        b->state_counts = new_state_counts;

        int32_t *new_goto_starts = ncc_alloc_array(int32_t, b->states_cap);
        memcpy(new_goto_starts, b->goto_starts, (size_t)old_cap * sizeof(int32_t));
        ncc_free(b->goto_starts);
        b->goto_starts = new_goto_starts;

        int32_t *new_goto_counts = ncc_alloc_array(int32_t, b->states_cap);
        memcpy(new_goto_counts, b->goto_counts, (size_t)old_cap * sizeof(int32_t));
        ncc_free(b->goto_counts);
        b->goto_counts = new_goto_counts;
    }

    while (b->state_items_total + count > b->state_items_cap) {
        int32_t  old_si_cap = b->state_items_cap;
        b->state_items_cap *= 2;

        int32_t *new_si = ncc_alloc_array(int32_t, b->state_items_cap);
        memcpy(new_si, b->state_items, (size_t)old_si_cap * sizeof(int32_t));
        ncc_free(b->state_items);
        b->state_items = new_si;
    }

    b->state_starts[sid] = b->state_items_total;
    b->state_counts[sid] = count;
    memcpy(&b->state_items[b->state_items_total],
           items_buf,
           (size_t)count * sizeof(int32_t));
    b->state_items_total += count;

    b->goto_starts[sid] = 0;
    b->goto_counts[sid] = 0;

    // Insert into dedup table, growing if needed.
    if (b->states_count * 4 >= b->dedup_cap * 3) {
        int32_t  old_dcap  = b->dedup_cap;
        int64_t *old_dhash = b->dedup_hash;
        int32_t *old_dsid  = b->dedup_sid;

        b->dedup_cap *= 2;
        b->dedup_hash = ncc_alloc_array(int64_t, b->dedup_cap);
        b->dedup_sid  = ncc_alloc_array(int32_t, b->dedup_cap);

        int32_t dmask = b->dedup_cap - 1;

        for (int32_t i = 0; i < old_dcap; i++) {
            if (old_dhash[i]) {
                int32_t di = (int32_t)((uint64_t)old_dhash[i] & (uint64_t)dmask);

                while (b->dedup_hash[di]) {
                    di = (di + 1) & dmask;
                }

                b->dedup_hash[di] = old_dhash[i];
                b->dedup_sid[di]  = old_dsid[i];
            }
        }

        ncc_free(old_dhash);
        ncc_free(old_dsid);

        idx = (int32_t)(h & (uint64_t)(b->dedup_cap - 1));

        while (b->dedup_hash[idx]) {
            idx = (idx + 1) & (b->dedup_cap - 1);
        }
    }

    b->dedup_hash[idx] = (int64_t)h;

    if (!b->dedup_hash[idx]) {
        b->dedup_hash[idx] = 1;
    }

    b->dedup_sid[idx] = sid;

    return sid;
}

// Build the LR(0) automaton.
static void
build_lr0_states(ncc_grammar_t *g)
{
    size_t n_rules = g->rules.len;
    size_t n_nts   = g->nt_list.len;

    if (n_rules == 0 || n_nts == 0) {
        return;
    }

    int32_t bitset_words = (g->lr0_item_count + 63) / 64;

    lr0_builder_t b = {
        .g                 = g,
        .states_cap        = 256,
        .states_count      = 0,
        .state_items_cap   = 4096,
        .state_items_total = 0,
        .gotos_cap         = 1024,
        .gotos_count       = 0,
        .dedup_cap         = 1024,
    };

    b.state_starts = ncc_alloc_array(int32_t, b.states_cap);
    b.state_counts = ncc_alloc_array(int32_t, b.states_cap);
    b.state_items  = ncc_alloc_array(int32_t, b.state_items_cap);
    b.goto_starts  = ncc_alloc_array(int32_t, b.states_cap);
    b.goto_counts  = ncc_alloc_array(int32_t, b.states_cap);
    b.gotos        = ncc_alloc_array(ncc_lr0_goto_t, b.gotos_cap);
    b.dedup_hash   = ncc_alloc_array(int64_t, b.dedup_cap);
    b.dedup_sid    = ncc_alloc_array(int32_t, b.dedup_cap);

    uint64_t *bits     = ncc_alloc_array(uint64_t, bitset_words);
    int32_t  *itemlist = ncc_alloc_array(int32_t, g->lr0_item_count);

    // Create initial state: closure of start NT's rules at dot=0.
    {
        ncc_nonterm_t *start = ncc_get_nonterm(g, g->default_start);

        memset(bits, 0, (size_t)bitset_words * sizeof(uint64_t));

        int32_t len = 0;

        for (size_t r = 0; r < start->rule_ids.len; r++) {
            int32_t rix  = start->rule_ids.data[r];
            int32_t item = g->lr0_rule_item_base[rix];

            size_t   w    = (size_t)item / 64;
            uint64_t mask = (uint64_t)1 << ((size_t)item % 64);

            if (!(bits[w] & mask)) {
                bits[w] |= mask;
                itemlist[len++] = item;
            }
        }

        lr0_closure(g, bits, itemlist, &len);
        qsort(itemlist, (size_t)len, sizeof(int32_t), cmp_int32);

        g->lr0_start_state = lr0_find_or_create(&b, itemlist, len);
    }

    int32_t sym_buf_sz = g->lr0_item_count > 256 ? g->lr0_item_count : 256;
    int32_t *sym_buf   = ncc_alloc_array(int32_t, sym_buf_sz);
    int32_t *kernel    = ncc_alloc_array(int32_t, g->lr0_item_count);

    for (int32_t si = 0; si < b.states_count; si++) {
        int32_t s_off = b.state_starts[si];
        int32_t s_cnt = b.state_counts[si];

        int32_t n_syms = 0;

        for (int32_t j = 0; j < s_cnt; j++) {
            ncc_lr0_item_t *it = &g->lr0_items[b.state_items[s_off + j]];

            if (it->dot >= it->rule_len) {
                continue;
            }

            ncc_parse_rule_t *rule = ncc_get_rule(g, it->rule_ix);
            ncc_match_t      *m    = &rule->contents.data[it->dot];
            int32_t            sym  = lr0_symbol_of_match(m);

            bool found = false;

            for (int32_t k = 0; k < n_syms; k++) {
                if (sym_buf[k] == sym) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                sym_buf[n_syms++] = sym;
            }
        }

        b.goto_starts[si] = b.gotos_count;

        for (int32_t s = 0; s < n_syms; s++) {
            int32_t sym = sym_buf[s];

            if (sym == -2) {
                continue;
            }

            memset(bits, 0, (size_t)bitset_words * sizeof(uint64_t));
            int32_t klen = 0;

            for (int32_t j = 0; j < s_cnt; j++) {
                ncc_lr0_item_t *it = &g->lr0_items[b.state_items[s_off + j]];

                if (it->dot >= it->rule_len) {
                    continue;
                }

                ncc_parse_rule_t *rule = ncc_get_rule(g, it->rule_ix);
                ncc_match_t      *m    = &rule->contents.data[it->dot];

                if (lr0_symbol_of_match(m) != sym) {
                    continue;
                }

                int32_t next_item = b.state_items[s_off + j] + 1;

                size_t   w    = (size_t)next_item / 64;
                uint64_t mask = (uint64_t)1 << ((size_t)next_item % 64);

                if (!(bits[w] & mask)) {
                    bits[w] |= mask;
                    kernel[klen++] = next_item;
                }
            }

            if (klen == 0) {
                continue;
            }

            lr0_closure(g, bits, kernel, &klen);
            qsort(kernel, (size_t)klen, sizeof(int32_t), cmp_int32);

            int32_t dest = lr0_find_or_create(&b, kernel, klen);

            if (b.gotos_count >= b.gotos_cap) {
                int32_t          old_gcap = b.gotos_cap;
                b.gotos_cap *= 2;
                ncc_lr0_goto_t *new_gotos = ncc_alloc_array(ncc_lr0_goto_t, b.gotos_cap);
                memcpy(new_gotos, b.gotos, (size_t)old_gcap * sizeof(ncc_lr0_goto_t));
                ncc_free(b.gotos);
                b.gotos = new_gotos;
            }

            b.gotos[b.gotos_count++] = (ncc_lr0_goto_t){
                .symbol     = sym,
                .dest_state = dest,
            };
        }

        b.goto_counts[si] = b.gotos_count - b.goto_starts[si];
    }

    // Flatten into grammar struct.
    g->lr0_state_count = b.states_count;
    g->lr0_states      = ncc_alloc_array(ncc_lr0_state_t, b.states_count);

    for (int32_t i = 0; i < b.states_count; i++) {
        g->lr0_states[i] = (ncc_lr0_state_t){
            .items_start = b.state_starts[i],
            .items_count = b.state_counts[i],
            .gotos_start = b.goto_starts[i],
            .gotos_count = b.goto_counts[i],
        };
    }

    g->lr0_state_items_total = b.state_items_total;
    g->lr0_state_items       = b.state_items;

    g->lr0_goto_count = b.gotos_count;
    g->lr0_gotos      = b.gotos;

    ncc_free(b.state_starts);
    ncc_free(b.state_counts);
    ncc_free(b.goto_starts);
    ncc_free(b.goto_counts);
    ncc_free(b.dedup_hash);
    ncc_free(b.dedup_sid);
    ncc_free(bits);
    ncc_free(itemlist);
    ncc_free(sym_buf);
    ncc_free(kernel);
}

// For each NT, find the LR(0) state that corresponds to the closure of
// that NT's rules at dot=0.
static void
lr0_compute_predict_states(ncc_grammar_t *g)
{
    size_t n_nts = g->nt_list.len;

    g->lr0_predict_state = ncc_alloc_array(int32_t, n_nts);

    int32_t   bitset_words = (g->lr0_item_count + 63) / 64;
    uint64_t *bits         = ncc_alloc_array(uint64_t, bitset_words);
    int32_t  *itemlist     = ncc_alloc_array(int32_t, g->lr0_item_count);

    for (size_t i = 0; i < n_nts; i++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, (int64_t)i);

        memset(bits, 0, (size_t)bitset_words * sizeof(uint64_t));
        int32_t len = 0;

        for (size_t r = 0; r < nt->rule_ids.len; r++) {
            int32_t rix  = nt->rule_ids.data[r];
            int32_t item = g->lr0_rule_item_base[rix];

            size_t   w    = (size_t)item / 64;
            uint64_t mask = (uint64_t)1 << ((size_t)item % 64);

            if (!(bits[w] & mask)) {
                bits[w] |= mask;
                itemlist[len++] = item;
            }
        }

        lr0_closure(g, bits, itemlist, &len);
        qsort(itemlist, (size_t)len, sizeof(int32_t), cmp_int32);

        // Find matching state by comparing sorted item lists.
        int32_t found = -1;

        for (int32_t s = 0; s < g->lr0_state_count; s++) {
            ncc_lr0_state_t *st = &g->lr0_states[s];

            if (lr0_items_eq(itemlist,
                             len,
                             &g->lr0_state_items[st->items_start],
                             st->items_count)) {
                found = s;
                break;
            }
        }

        g->lr0_predict_state[i] = found;
    }

    ncc_free(bits);
    ncc_free(itemlist);
}

// ============================================================================
// Finalization
// ============================================================================

void
ncc_grammar_finalize(ncc_grammar_t *g)
{
    if (g->finalized) {
        return;
    }

    size_t n = g->nt_list.len;

    for (size_t i = 0; i < n; i++) {
        ncc_list_t(int) stack = ncc_list_new(int);
        is_nullable_nt(g, (int64_t)i, &stack);
        ncc_list_free(stack);
    }

    if (g->error_rules) {
        n = g->rules.len;

        for (size_t i = 0; i < n; i++) {
            create_one_error_rule_set(g, (int32_t)i);
        }
    }

    compute_all_first_sets(g);
    compute_left_corners(g);

    compute_lr0_items(g);
    build_lr0_states(g);
    lr0_compute_predict_states(g);

    // Check if the grammar has group NTs (BSR reconstruction doesn't
    // handle multi-match groups yet, so BSR emission is skipped).
    n = g->nt_list.len;

    for (size_t i = 0; i < n; i++) {
        if (g->nt_list.data[i].group_nt) {
            g->has_groups = true;
            break;
        }
    }

    ncc_nonterm_t *start = ncc_get_nonterm(g, g->default_start);

    if (start) {
        start->start_nt = true;
    }

    g->finalized = true;
}
