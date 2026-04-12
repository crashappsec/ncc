// pwz.c - Parsing With Zippers (PWZ): derivative-based parser.
//
// Translates Darragh & Adams (ICFP 2020) from OCaml to C, integrating
// with n00b's grammar, tree, and walk-action infrastructure.
//
// The algorithm uses generalized zippers to traverse grammar expressions,
// handling arbitrary CFGs including ambiguous and left-recursive grammars.

#include "parse/pwz.h"
#include "parse/parse_tree.h"
#include "internal/parse/pwz_internal.h"
#include "internal/parse/grammar_internal.h"
#include "internal/parse/unicode_class.h"
#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "scanner/token_stream.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Token access helpers (via shared stream)
// ============================================================================

static inline ncc_token_info_t *
get_token(ncc_pwz_parser_t *p, int32_t pos)
{
    return ncc_stream_get(p->stream, pos);
}

// ============================================================================
// Per-parse allocation helpers
// ============================================================================

#ifdef NCC_MEM_DEBUG
typedef struct {
    size_t mem_count;
    size_t cxt_count;
    size_t cxt_node_count;
    size_t result_exp_count;
    size_t child_array_count;
    size_t child_array_bytes;
} pwz_alloc_stats_t;

static pwz_alloc_stats_t pwz_stats;
#endif

static pwz_mem_t *
alloc_mem(ncc_pwz_parser_t *p)
{
    pwz_mem_t *m;

    if (p->free_mems) {
        m = p->free_mems;
        p->free_mems = (pwz_mem_t *)m->parents;
    }
    else {
        m = ncc_arena_alloc(&p->parse_arena, sizeof(pwz_mem_t));
    }

    m->parents   = nullptr;
    m->result    = p->exp_bottom;
    m->start_pos = PWZ_POS_BOTTOM;
    m->end_pos   = PWZ_POS_BOTTOM;
    m->refcount  = 1;

    return m;
}

static inline pwz_mem_t *
mem_retain(pwz_mem_t *m)
{
    if (m) {
        m->refcount++;
    }
    return m;
}

static void mem_release(ncc_pwz_parser_t *p, pwz_mem_t *m);

// Release a context's memo reference.
static inline void
cxt_release_mem(ncc_pwz_parser_t *p, pwz_cxt_t *c)
{
    switch (c->kind) {
    case PWZ_CXT_SEQ: mem_release(p, c->seq.mem); break;
    case PWZ_CXT_ALT: mem_release(p, c->alt.mem); break;
    case PWZ_CXT_TOP: break;
    }
}

static void
mem_release(ncc_pwz_parser_t *p, pwz_mem_t *m)
{
    if (!m || --m->refcount > 0) {
        return;
    }
    // Cascade: release all contexts in the parent chain,
    // which in turn release the memos they reference.
    pwz_cxt_node_t *node = m->parents;
    while (node) {
        pwz_cxt_node_t *next = node->next;
        cxt_release_mem(p, node->cxt);
        node = next;
    }
    // Return to free list.
    m->parents = (pwz_cxt_node_t *)p->free_mems;
    p->free_mems = m;
}

static pwz_cxt_t *
alloc_cxt(ncc_pwz_parser_t *p)
{
#ifdef NCC_MEM_DEBUG
    pwz_stats.cxt_count++;
#endif
    pwz_cxt_t *c = ncc_arena_alloc(&p->parse_arena, sizeof(pwz_cxt_t));
    *c = (pwz_cxt_t){0};
    return c;
}

static pwz_cxt_node_t *
alloc_cxt_node(ncc_pwz_parser_t *p, pwz_cxt_t *cxt, pwz_cxt_node_t *next)
{
    pwz_cxt_node_t *n = ncc_arena_alloc(&p->parse_arena, sizeof(pwz_cxt_node_t));

    n->cxt  = cxt;
    n->next = next;

#ifdef NCC_MEM_DEBUG
    pwz_stats.cxt_node_count++;
#endif
    return n;
}

static pwz_exp_t *
alloc_result_exp(ncc_pwz_parser_t *p)
{
#ifdef NCC_MEM_DEBUG
    pwz_stats.result_exp_count++;
#endif
    return ncc_arena_alloc(&p->parse_arena, sizeof(pwz_exp_t));
}

#ifdef NCC_MEM_DEBUG
static void
pwz_track_child_array(size_t bytes)
{
    pwz_stats.child_array_count++;
    pwz_stats.child_array_bytes += bytes;
}

void
ncc_pwz_report_stats(void)
{
    size_t mem_bytes      = pwz_stats.mem_count * sizeof(pwz_mem_t);
    size_t cxt_bytes      = pwz_stats.cxt_count * sizeof(pwz_cxt_t);
    size_t cxt_node_bytes = pwz_stats.cxt_node_count * sizeof(pwz_cxt_node_t);
    size_t exp_bytes      = pwz_stats.result_exp_count * sizeof(pwz_exp_t);
    size_t total = mem_bytes + cxt_bytes + cxt_node_bytes + exp_bytes
                 + pwz_stats.child_array_bytes;

    fprintf(stderr, "[pwz] mems=%zu(%zu) cxts=%zu(%zu) cxt_nodes=%zu(%zu) "
            "result_exps=%zu(%zu) child_arrays=%zu(%zu) total=%zu\n",
            pwz_stats.mem_count, mem_bytes,
            pwz_stats.cxt_count, cxt_bytes,
            pwz_stats.cxt_node_count, cxt_node_bytes,
            pwz_stats.result_exp_count, exp_bytes,
            pwz_stats.child_array_count, pwz_stats.child_array_bytes,
            total);

    pwz_stats = (pwz_alloc_stats_t){0};
}
#endif

// ============================================================================
// Track grammar exp nodes for per-parse memo reset
// ============================================================================

static void
register_exp(ncc_pwz_parser_t *p, pwz_exp_t *e)
{
    ncc_list_push(p->all_exps, e);
}

// ============================================================================
// Grammar -> Exp conversion
// ============================================================================

static pwz_exp_t *
make_tok_exp(ncc_pwz_parser_t *p, int64_t tid)
{
    pwz_exp_t *e = ncc_alloc(pwz_exp_t);

    e->kind    = PWZ_TOK;
    e->tid = tid;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_class_exp(ncc_pwz_parser_t *p, ncc_char_class_t cc)
{
    pwz_exp_t *e = ncc_alloc(pwz_exp_t);

    e->kind   = PWZ_CLASS;
    e->cc = cc;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_any_exp(ncc_pwz_parser_t *p)
{
    pwz_exp_t *e = ncc_alloc(pwz_exp_t);

    e->kind = PWZ_ANY;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_seq_exp(ncc_pwz_parser_t *p,
             const char        *name,
             int64_t            nt_id,
             int32_t            rule_ix,
             pwz_exp_ptr_t     *children,
             int32_t            nchildren)
{
    pwz_exp_t *e = ncc_alloc(pwz_exp_t);

    e->kind          = PWZ_SEQ;
    e->seq.name      = name;
    e->nt_id     = nt_id;
    e->rule_ix   = rule_ix;
    e->seq.children  = children;
    e->nchildren = nchildren;

    register_exp(p, e);
    return e;
}

static pwz_exp_t *
make_alt_exp(ncc_pwz_parser_t *p, int64_t nt_id)
{
    pwz_exp_t *e = ncc_alloc(pwz_exp_t);

    e->kind      = PWZ_ALT;
    e->nt_id = nt_id;
    e->alt.alts  = ncc_list_new(pwz_exp_ptr_t);

    register_exp(p, e);
    return e;
}

static void
alt_add(pwz_exp_t *alt, pwz_exp_t *child)
{
    ncc_list_push(alt->alt.alts, child);
}

// Build exp children from a rule's contents list, resolving NT references
// to the pre-allocated Alt nodes.
static void
build_seq_children(ncc_pwz_parser_t *p,
                   ncc_parse_rule_t *rule,
                   pwz_exp_ptr_t    **out_children,
                   int32_t           *out_count)
{
    size_t n = ncc_list_len(rule->contents);

    // Count non-empty items first.
    int32_t count = 0;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *item = &rule->contents.data[i];

        if (item->kind != NCC_MATCH_EMPTY) {
            count++;
        }
    }

    if (count == 0) {
        *out_children = nullptr;
        *out_count    = 0;
        return;
    }

    pwz_exp_ptr_t *children = ncc_alloc_array(pwz_exp_ptr_t, count);
    int32_t        ix       = 0;

    for (size_t i = 0; i < n; i++) {
        ncc_match_t *item = &rule->contents.data[i];

        switch (item->kind) {
        case NCC_MATCH_NT:
            children[ix++] = p->nt_exps[item->nt_id];
            break;

        case NCC_MATCH_TERMINAL:
            children[ix++] = make_tok_exp(p, item->terminal_id);
            break;

        case NCC_MATCH_CLASS:
            children[ix++] = make_class_exp(p, item->char_class);
            break;

        case NCC_MATCH_ANY:
            children[ix++] = make_any_exp(p);
            break;

        case NCC_MATCH_GROUP: {
            ncc_rule_group_t *grp = (ncc_rule_group_t *)item->group;
            children[ix++]         = p->nt_exps[grp->contents_id];
            break;
        }

        case NCC_MATCH_EMPTY:
        case NCC_MATCH_SET:
            break;
        }
    }

    *out_children = children;
    *out_count    = ix;
}

// Handle EBNF group expansion for group NTs.
static void
expand_group_nt(ncc_pwz_parser_t *p, ncc_grammar_t *g, int64_t nt_id)
{
    ncc_nonterm_t *nt  = ncc_get_nonterm(g, nt_id);
    pwz_exp_t      *alt = p->nt_exps[nt_id];

    // Find the rule_group that uses this NT as its contents.
    ncc_rule_group_t *grp = nullptr;

    for (size_t i = 0; i < ncc_list_len(g->rules); i++) {
        ncc_parse_rule_t *rule = &g->rules.data[i];

        for (size_t j = 0; j < ncc_list_len(rule->contents); j++) {
            ncc_match_t *item = &rule->contents.data[j];

            if (item->kind == NCC_MATCH_GROUP) {
                ncc_rule_group_t *rg = (ncc_rule_group_t *)item->group;

                if (rg->contents_id == nt_id) {
                    grp = rg;
                    break;
                }
            }
        }

        if (grp) {
            break;
        }
    }

    if (!grp) {
        // Couldn't find group info. Just build normally from rules.
        for (size_t i = 0; i < ncc_list_len(nt->rule_ids); i++) {
            int32_t            rule_ix = nt->rule_ids.data[i];
            ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);

            if (rule->penalty_rule) {
                continue;
            }

            pwz_exp_ptr_t *children;
            int32_t        nchildren;

            build_seq_children(p, rule, &children, &nchildren);

            pwz_exp_t *seq = make_seq_exp(p, nt->name.data, nt_id, (int32_t)i,
                                          children, nchildren);
            alt_add(alt, seq);
        }

        return;
    }

    // Build body alt from the group NT's own rules.
    pwz_exp_t *body_alt = make_alt_exp(p, nt_id);

    for (size_t i = 0; i < ncc_list_len(nt->rule_ids); i++) {
        int32_t            rule_ix = nt->rule_ids.data[i];
        ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);

        if (rule->penalty_rule) {
            continue;
        }

        pwz_exp_ptr_t *children;
        int32_t        nchildren;

        build_seq_children(p, rule, &children, &nchildren);

        pwz_exp_t *seq = make_seq_exp(p, nt->name.data, nt_id, (int32_t)i,
                                      children, nchildren);
        alt_add(body_alt, seq);
    }

    // Empty seq (matches epsilon).
    pwz_exp_t *empty_seq = make_seq_exp(p, nt->name.data, nt_id, -1, nullptr, 0);

    size_t body_nalts = ncc_list_len(body_alt->alt.alts);

    if (grp->min == 0 && grp->max == 1) {
        // Optional: Alt(body, empty)
        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }

        alt_add(alt, empty_seq);
    }
    else if (grp->min == 0 && grp->max == 0) {
        // Star (left-recursive): Alt(Seq(self, body), empty)
        // Left-recursion lets PWZ's seed-growing handle repetition
        // with a single memo, avoiding O(n) parent-chain depth.
        for (size_t i = 0; i < body_nalts; i++) {
            pwz_exp_t *body_seq = body_alt->alt.alts.data[i];
            int32_t    nc       = body_seq->nchildren;
            int32_t    new_nc   = nc + 1;

            pwz_exp_ptr_t *new_children = ncc_alloc_array(pwz_exp_ptr_t, new_nc);
            new_children[0] = alt; // self-reference (left-recursive)
            memcpy(new_children + 1, body_seq->seq.children,
                   (size_t)nc * sizeof(pwz_exp_ptr_t));

            pwz_exp_t *rep_seq = make_seq_exp(p, nt->name.data, nt_id,
                                              body_seq->rule_ix,
                                              new_children, new_nc);
            alt_add(alt, rep_seq);
        }

        alt_add(alt, empty_seq);
    }
    else if (grp->min == 1 && grp->max == 0) {
        // Plus (left-recursive): Alt(Seq(self, body), body)
        // Left-recursion lets PWZ's seed-growing handle repetition
        // with a single memo, avoiding O(n) parent-chain depth.
        for (size_t i = 0; i < body_nalts; i++) {
            pwz_exp_t *body_seq = body_alt->alt.alts.data[i];
            int32_t    nc       = body_seq->nchildren;
            int32_t    new_nc   = nc + 1;

            pwz_exp_ptr_t *new_children = ncc_alloc_array(pwz_exp_ptr_t, new_nc);
            new_children[0] = alt; // self-reference (left-recursive)
            memcpy(new_children + 1, body_seq->seq.children,
                   (size_t)nc * sizeof(pwz_exp_ptr_t));

            pwz_exp_t *rep_seq = make_seq_exp(p, nt->name.data, nt_id,
                                              body_seq->rule_ix,
                                              new_children, new_nc);
            alt_add(alt, rep_seq);
        }

        // Base case: just the body itself.
        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }
    }
    else {
        // General case: just use the body rules directly.
        for (size_t i = 0; i < body_nalts; i++) {
            alt_add(alt, body_alt->alt.alts.data[i]);
        }
    }
}

static void
build_exp_graph(ncc_pwz_parser_t *p, ncc_grammar_t *g)
{
    int32_t num_nts = (int32_t)ncc_list_len(g->nt_list);

    p->nt_exps = ncc_alloc_array(pwz_exp_ptr_t, num_nts);

    // Phase 1: Create one Alt node per NT (handles forward refs / cycles).
    for (int32_t i = 0; i < num_nts; i++) {
        p->nt_exps[i] = make_alt_exp(p, i);
    }

    // Phase 2: Populate each Alt with Seq children from rules.
    for (int32_t i = 0; i < num_nts; i++) {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, i);

        if (nt->group_nt) {
            expand_group_nt(p, g, i);
            continue;
        }

        pwz_exp_t *alt_node = p->nt_exps[i];

        for (size_t j = 0; j < ncc_list_len(nt->rule_ids); j++) {
            int32_t            rule_ix = nt->rule_ids.data[j];
            ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);

            if (rule->penalty_rule) {
                continue;
            }

            pwz_exp_ptr_t *children;
            int32_t        nchildren;

            build_seq_children(p, rule, &children, &nchildren);

            pwz_exp_t *seq = make_seq_exp(p, nt->name.data, (int64_t)i,
                                          (int32_t)j, children, nchildren);
            alt_add(alt_node, seq);
        }
    }

    p->start_exp = p->nt_exps[g->default_start];
}

// ============================================================================
// Per-parse memo initialization / reset
// ============================================================================

static void
reset_memos(ncc_pwz_parser_t *p)
{
    size_t num = ncc_list_len(p->all_exps);

    for (size_t i = 0; i < num; i++) {
        mem_release(p, p->all_exps.data[i]->mem);
        p->all_exps.data[i]->mem = nullptr;
    }
}

// ============================================================================
// Core derive: d_d, d_d_prime, d_u, d_u_prime
// ============================================================================

// Encode a terminal ID as a void* for FIRST-set lookups.
#define TERM_TO_PTR(id) ((void *)(uintptr_t)((uint64_t)(id) + 0x100))

static inline bool
nt_first_matches(ncc_nonterm_t *nt, int64_t token_id)
{
    if (nt->first_has_any) {
        return true;
    }

    if (!nt->first_set || nt->first_set->count == 0) {
        return true;
    }

    return ncc_dict_contains(nt->first_set, TERM_TO_PTR(token_id));
}

static inline bool
rule_first_matches(ncc_parse_rule_t *rule, int64_t token_id)
{
    if (rule->first_has_any) {
        return true;
    }

    if (!rule->first_set || rule->first_set->count == 0) {
        return true;
    }

    return ncc_dict_contains(rule->first_set, TERM_TO_PTR(token_id));
}

// ============================================================================
// Core derive functions
// ============================================================================

static void
d_d(ncc_pwz_parser_t *p, int32_t pos, ncc_token_info_t *tok,
    pwz_cxt_t *cxt, pwz_exp_t *exp);
static void
d_d_prime(ncc_pwz_parser_t *p, int32_t pos, ncc_token_info_t *tok,
          pwz_mem_t *mem, pwz_exp_t *exp);
static void
d_u(ncc_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_mem_t *mem);
static void
d_u_prime(ncc_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_cxt_t *cxt);

static bool
token_matches(ncc_token_info_t *tok, pwz_exp_t *exp)
{
    if (!tok) {
        return false;
    }

    switch (exp->kind) {
    case PWZ_TOK:
        return tok->tid == (int32_t)exp->tid;

    case PWZ_CLASS:
        return ncc_codepoint_matches_class(tok->tid, exp->cc);

    case PWZ_ANY:
        return true;

    default:
        return false;
    }
}

static void
d_d(ncc_pwz_parser_t *p, int32_t pos, ncc_token_info_t *tok,
    pwz_cxt_t *cxt, pwz_exp_t *exp)
{
    if (exp->mem && exp->mem->start_pos == pos) {
        exp->mem->parents = alloc_cxt_node(p, cxt, exp->mem->parents);

        if (pwz_mem_end_pos(exp->mem) != PWZ_POS_BOTTOM) {
            d_u_prime(p, pwz_mem_end_pos(exp->mem), exp->mem->result, cxt);
        }

        return;
    }

    mem_release(p, exp->mem);  // drop old memo ref (if any)

    pwz_mem_t *mem = alloc_mem(p);
#ifdef NCC_MEM_DEBUG
    pwz_stats.mem_count++;
#endif

    mem->start_pos = pos;
    mem->parents   = alloc_cxt_node(p, cxt, nullptr);
    exp->mem       = mem;  // takes the initial refcount=1

    d_d_prime(p, pos, tok, mem, exp);
}

static void
d_d_prime(ncc_pwz_parser_t *p, int32_t pos, ncc_token_info_t *tok,
          pwz_mem_t *mem, pwz_exp_t *exp)
{
    switch (exp->kind) {
    case PWZ_TOK:
    case PWZ_CLASS:
    case PWZ_ANY:
        if (token_matches(tok, exp)) {
            pwz_exp_t *result = alloc_result_exp(p);

            result->kind    = exp->kind;
            result->tid = tok->tid;

            ncc_list_push(p->worklist_swap,
                           ((pwz_zipper_t){.result = result,
                                           .mem = mem_retain(mem)}));
        }

        break;

    case PWZ_SEQ:
        if (exp->nchildren == 0) {
            pwz_exp_t *result = alloc_result_exp(p);

            result->kind          = PWZ_SEQ;
            result->seq.name      = exp->seq.name;
            result->nt_id     = exp->nt_id;
            result->rule_ix   = exp->rule_ix;
            result->seq.children  = nullptr;
            result->nchildren = 0;

            d_u(p, pos, result, mem);
        }
        else {
            // Store parent memo directly on SeqC; on completion,
            // d_u_prime routes directly to parent_mem.
            pwz_cxt_t *seq_cxt = alloc_cxt(p);

            seq_cxt->kind        = PWZ_CXT_SEQ;
            seq_cxt->seq.mem     = mem_retain(mem);
            seq_cxt->nt_id   = exp->nt_id;
            seq_cxt->rule_ix = exp->rule_ix;
            seq_cxt->seq.left    = nullptr;
            seq_cxt->nleft   = 0;
            seq_cxt->seq.right   = exp->seq.children + 1;
            seq_cxt->seq.nright  = exp->nchildren - 1;

            d_d(p, pos, tok, seq_cxt, exp->seq.children[0]);
        }

        break;

    case PWZ_ALT: {
        // FIRST-set filtering.
        if (tok && exp->nt_id >= 0) {
            ncc_nonterm_t *nt = ncc_get_nonterm(p->grammar, exp->nt_id);

            if (nt && !nt->group_nt && !nt_first_matches(nt, tok->tid)) {
                break;
            }
        }

        bool can_filter_alts = false;

        if (tok && exp->nt_id >= 0) {
            ncc_nonterm_t *nt = ncc_get_nonterm(p->grammar, exp->nt_id);

            if (nt && !nt->group_nt) {
                can_filter_alts = true;
            }
        }

        size_t nalts = ncc_list_len(exp->alt.alts);

        for (size_t i = 0; i < nalts; i++) {
            pwz_exp_t *alt_child = exp->alt.alts.data[i];

            if (can_filter_alts && alt_child->kind == PWZ_SEQ
                && alt_child->nt_id >= 0 && alt_child->rule_ix >= 0) {
                ncc_nonterm_t *nt = ncc_get_nonterm(p->grammar,
                                                      alt_child->nt_id);

                if (nt && alt_child->rule_ix < (int32_t)ncc_list_len(nt->rule_ids)) {
                    int32_t            rix  = nt->rule_ids.data[alt_child->rule_ix];
                    ncc_parse_rule_t *rule = ncc_get_rule(p->grammar, rix);

                    if (rule && !rule_first_matches(rule, tok->tid)) {
                        continue;
                    }
                }
            }

            pwz_cxt_t *alt_cxt = alloc_cxt(p);

            alt_cxt->kind    = PWZ_CXT_ALT;
            alt_cxt->alt.mem = mem_retain(mem);

            d_d(p, pos, tok, alt_cxt, alt_child);
        }

        break;
    }
    }
}

static void
d_u(ncc_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_mem_t *mem)
{
    int32_t ep = pwz_mem_end_pos(mem);

    if (ep != PWZ_POS_BOTTOM) {
        if (pos == ep) {
            // Same-position completion: accumulate ambiguity.
            pwz_exp_t *existing = mem->result;

            if (existing->kind == PWZ_ALT && existing->nt_id == -1) {
                ncc_list_push(existing->alt.alts, result);
            }
            else {
                pwz_exp_t *copy = alloc_result_exp(p);

                *copy = *existing;

                existing->kind      = PWZ_ALT;
                existing->nt_id = -1;
                existing->alt.alts  = ncc_list_new(pwz_exp_ptr_t);
                ncc_list_push(existing->alt.alts, copy);
                ncc_list_push(existing->alt.alts, result);
            }

            return;
        }

        // Later-position completion (left-recursion grew the seed).
        // Skip if already propagating this memo (re-entrant guard).
        if (pwz_mem_in_progress(mem)) {
            return;
        }
    }

    // First completion, or longer left-recursive match.
    // Paper: m.end_pos <- p; m.result <- e; propagate to all parents.
    mem->end_pos = pos | PWZ_MEM_IN_PROGRESS_BIT;
    mem->result  = result;

    pwz_cxt_node_t *node = mem->parents;

    while (node) {
        d_u_prime(p, pos, result, node->cxt);
        node = node->next;
    }

    pwz_mem_set_in_progress(mem, false);
}

static void
d_u_prime(ncc_pwz_parser_t *p, int32_t pos, pwz_exp_t *result, pwz_cxt_t *cxt)
{
    switch (cxt->kind) {
    case PWZ_CXT_TOP:
        ncc_list_push(p->tops, result);
        break;

    case PWZ_CXT_SEQ: {
        if (cxt->seq.nright == 0) {
            int32_t total = cxt->nleft + 1;

            pwz_exp_ptr_t *children = ncc_arena_alloc(
                &p->parse_arena, (size_t)total * sizeof(pwz_exp_ptr_t));
#ifdef NCC_MEM_DEBUG
            pwz_track_child_array((size_t)total * sizeof(pwz_exp_ptr_t));
#endif

            for (int32_t i = 0; i < cxt->nleft; i++) {
                children[i] = cxt->seq.left[i];
            }

            children[cxt->nleft] = result;

            pwz_exp_t *seq_result = alloc_result_exp(p);

            seq_result->kind          = PWZ_SEQ;
            seq_result->seq.name      = cxt->nt_id >= 0
                ? ncc_get_nonterm(p->grammar, cxt->nt_id)->name.data
                : nullptr;
            seq_result->nt_id     = cxt->nt_id;
            seq_result->rule_ix   = cxt->rule_ix;
            seq_result->seq.children  = children;
            seq_result->nchildren = total;

            d_u(p, pos, seq_result, cxt->seq.mem);
        }
        else {
            // Paper: d_d (SeqC (m, s, e :: es_L, es_R)) e_R
            // Reuses the same memo m; no extra AltC wrapper.
            int32_t new_nleft = cxt->nleft + 1;

            pwz_exp_ptr_t *new_left = ncc_arena_alloc(
                &p->parse_arena, (size_t)new_nleft * sizeof(pwz_exp_ptr_t));
#ifdef NCC_MEM_DEBUG
            pwz_track_child_array((size_t)new_nleft * sizeof(pwz_exp_ptr_t));
#endif

            for (int32_t i = 0; i < cxt->nleft; i++) {
                new_left[i] = cxt->seq.left[i];
            }

            new_left[cxt->nleft] = result;

            pwz_cxt_t *new_seq_cxt = alloc_cxt(p);

            new_seq_cxt->kind        = PWZ_CXT_SEQ;
            new_seq_cxt->seq.mem     = mem_retain(cxt->seq.mem);
            new_seq_cxt->nt_id   = cxt->nt_id;
            new_seq_cxt->rule_ix = cxt->rule_ix;
            new_seq_cxt->seq.left    = new_left;
            new_seq_cxt->nleft   = new_nleft;
            new_seq_cxt->seq.right   = cxt->seq.right + 1;
            new_seq_cxt->seq.nright  = cxt->seq.nright - 1;

            d_d(p, pos, get_token(p, pos), new_seq_cxt, cxt->seq.right[0]);
        }

        break;
    }

    case PWZ_CXT_ALT:
        d_u(p, pos, result, cxt->alt.mem);
        break;
    }
}

// ============================================================================
// Parse loop
// ============================================================================

static void
init_parse(ncc_pwz_parser_t *p)
{
    reset_memos(p);
    ncc_list_clear(p->worklist);
    ncc_list_clear(p->worklist_swap);
    ncc_list_clear(p->tops);
    p->result_tree  = nullptr;
    p->result_trees = (ncc_parse_tree_array_t){0};

    pwz_mem_t *mem_top = alloc_mem(p);
#ifdef NCC_MEM_DEBUG
    pwz_stats.mem_count++;
#endif

    mem_top->start_pos = 0;

    pwz_cxt_t *top_cxt = alloc_cxt(p);

    top_cxt->kind    = PWZ_CXT_TOP;
    mem_top->parents = alloc_cxt_node(p, top_cxt, nullptr);

    // Paper: let c = SeqC (m_top, s_bottom, [], [e; eof]) in
    //        let m_seq = { start_pos; parents = [c]; ... } in
    //        derive returns (e', m_seq)
    // We enter via d_d on the start expression with an AltC(m_top) parent,
    // since d_d' for the start_exp (an Alt) will distribute to alternatives.
    pwz_cxt_t *alt_cxt = alloc_cxt(p);

    alt_cxt->kind    = PWZ_CXT_ALT;
    alt_cxt->alt.mem = mem_retain(mem_top);

    ncc_token_info_t *tok = get_token(p, 0);

    d_d(p, 0, tok, alt_cxt, p->start_exp);
}

static bool
run_parse(ncc_pwz_parser_t *p)
{
    if (ncc_list_len(p->tops) > 0 && ncc_stream_token_count(p->stream) == 0) {
        return true;
    }

    for (int32_t pos = 0; ; pos++) {
        // Swap worklists.
        ncc_list_t(pwz_zipper_t) tmp = p->worklist;
        p->worklist      = p->worklist_swap;
        p->worklist_swap = tmp;
        ncc_list_clear(p->worklist_swap);
        ncc_list_clear(p->tops);

        size_t wl_len = ncc_list_len(p->worklist);

        if (wl_len == 0) {
            return false;
        }

        int32_t complete_pos = pos + 1;

        // Ensure the next token is available via lazy stream fill:
        // d_u may derive new items that call
        // d_d(p, complete_pos, get_token(p, complete_pos), ...),
        // so the token at complete_pos must be available.
        bool have_next = (ncc_stream_get(p->stream, complete_pos) != nullptr);

        for (size_t i = 0; i < wl_len; i++) {
            pwz_zipper_t *z = &p->worklist.data[i];
            d_u(p, complete_pos, z->result, z->mem);
        }

        // Release worklist memo refs now that all zippers are consumed.
        for (size_t i = 0; i < wl_len; i++) {
            mem_release(p, p->worklist.data[i].mem);
        }

        if (!have_next) {
            // No more tokens — this was the last position.
            return ncc_list_len(p->tops) > 0;
        }
    }
}

// ============================================================================
// Tree construction helpers
// ============================================================================

static ncc_parse_tree_t *
make_token_node(ncc_token_info_t *tok)
{
    return ncc_tree_leaf(ncc_nt_node_t, ncc_token_info_ptr_t, tok);
}

static ncc_parse_tree_t *
make_epsilon_node(int32_t pos)
{
    ncc_nt_node_t pn = {0};

    pn.name  = ncc_string_from_cstr("\xce\xb5"); // UTF-8 epsilon
    pn.id    = NCC_EMPTY_STRING;
    pn.start = pos;
    pn.end   = pos;

    return ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
}

static ncc_parse_tree_t *
make_nt_node(ncc_grammar_t *g, int64_t nt_id, int32_t rule_index,
             int32_t start, int32_t end)
{
    ncc_nonterm_t *nt = ncc_get_nonterm(g, nt_id);
    ncc_nt_node_t  pn = {0};

    pn.name       = (nt && nt->name.data) ? nt->name : ncc_string_from_cstr("?");
    pn.id         = nt_id;
    pn.rule_index = rule_index;
    pn.start      = start;
    pn.end        = end;

    return ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
}

static ncc_parse_tree_t *
make_group_node(const char *name, int32_t start, int32_t end)
{
    ncc_nt_node_t pn = {0};

    pn.name      = name ? ncc_string_from_cstr(name) : ncc_string_from_cstr("group");
    pn.id        = NCC_GROUP_ID;
    pn.group_top = true;
    pn.start     = start;
    pn.end       = end;

    return ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
}

// ============================================================================
// Result exp -> parse tree conversion
// ============================================================================

typedef struct {
    int32_t            pos;
    ncc_pwz_parser_t *parser;
} tree_convert_state_t;

static ncc_parse_tree_t *
convert_exp_to_tree(ncc_pwz_parser_t *p, pwz_exp_t *exp,
                    tree_convert_state_t *st)
{
    if (!exp || exp == p->exp_bottom) {
        return make_epsilon_node(st->pos);
    }

    switch (exp->kind) {
    case PWZ_TOK:
    case PWZ_CLASS:
    case PWZ_ANY: {
        ncc_token_info_t *tok = get_token(p, st->pos);

        if (!tok) {
            return make_epsilon_node(st->pos);
        }

        ncc_parse_tree_t *t = make_token_node(tok);

        st->pos++;
        return t;
    }

    case PWZ_SEQ: {
        int32_t start = st->pos;

        if (exp->nchildren == 0) {
            if (exp->nt_id >= 0) {
                ncc_nonterm_t *nt = ncc_get_nonterm(p->grammar, exp->nt_id);

                if (nt && nt->group_nt) {
                    return make_group_node(exp->seq.name, start, start);
                }

                return make_nt_node(p->grammar, exp->nt_id,
                                    exp->rule_ix, start, start);
            }

            return make_epsilon_node(start);
        }

        // Collect children into a temporary list.
        ncc_list_t(ncc_parse_tree_ptr_t) children
            = ncc_list_new_cap(ncc_parse_tree_ptr_t,
                                        exp->nchildren);

        for (int32_t i = 0; i < exp->nchildren; i++) {
            ncc_parse_tree_t *child
                = convert_exp_to_tree(p, exp->seq.children[i], st);
            ncc_list_push(children, child);
        }

        int32_t end = st->pos;

        ncc_parse_tree_t *tree;

        if (exp->nt_id >= 0) {
            ncc_nonterm_t *nt = ncc_get_nonterm(p->grammar, exp->nt_id);

            if (nt && nt->group_nt) {
                tree = make_group_node(exp->seq.name, start, end);
            }
            else {
                tree = make_nt_node(p->grammar, exp->nt_id,
                                    exp->rule_ix, start, end);
            }
        }
        else {
            size_t nch = ncc_list_len(children);

            if (nch == 1) {
                ncc_parse_tree_t *result = children.data[0];
                ncc_list_free(children);
                return result;
            }

            ncc_nt_node_t pn = {0};

            pn.name  = exp->seq.name ? ncc_string_from_cstr(exp->seq.name)
                                     : ncc_string_from_cstr("?");
            pn.id    = exp->nt_id;
            pn.start = start;
            pn.end   = end;
            tree = ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, pn);
        }

        size_t nch = ncc_list_len(children);

        for (size_t i = 0; i < nch; i++) {
            (void)ncc_tree_add_child(tree, children.data[i]);
        }

        ncc_list_free(children);

        return tree;
    }

    case PWZ_ALT: {
        size_t nalts = ncc_list_len(exp->alt.alts);

        if (nalts > 0) {
            return convert_exp_to_tree(p, exp->alt.alts.data[nalts - 1], st);
        }

        return make_epsilon_node(st->pos);
    }
    }

    return make_epsilon_node(st->pos);
}

static ncc_parse_tree_t *
build_result_tree(ncc_pwz_parser_t *p, pwz_exp_t *top_result)
{
    tree_convert_state_t st = {.pos = 0, .parser = p};

    return convert_exp_to_tree(p, top_result, &st);
}

// Count the number of trees in an ambiguity forest.
static int32_t
count_trees_in_exp(pwz_exp_t *exp)
{
    if (!exp) {
        return 1;
    }

    if (exp->kind == PWZ_ALT && exp->nt_id == -1) {
        int32_t total = 0;
        size_t  nalts = ncc_list_len(exp->alt.alts);

        for (size_t i = 0; i < nalts; i++) {
            total += count_trees_in_exp(exp->alt.alts.data[i]);
        }

        return total;
    }

    if (exp->kind == PWZ_SEQ) {
        int32_t product = 1;

        for (int32_t i = 0; i < exp->nchildren; i++) {
            product *= count_trees_in_exp(exp->seq.children[i]);
        }

        return product;
    }

    return 1;
}

static pwz_exp_t *
find_top_ambiguity(pwz_exp_t *exp)
{
    if (!exp) {
        return nullptr;
    }

    if (exp->kind == PWZ_ALT && exp->nt_id == -1) {
        return exp;
    }

    if (exp->kind == PWZ_SEQ && exp->nt_id == -1
        && exp->nchildren == 1) {
        return find_top_ambiguity(exp->seq.children[0]);
    }

    return nullptr;
}

static void
enumerate_trees(ncc_pwz_parser_t   *p,
                pwz_exp_t           *top_result,
                ncc_parse_tree_ptr_t **out,
                int32_t              *out_count)
{
    int32_t total = count_trees_in_exp(top_result);

    if (total <= 1) {
        *out_count = 1;
        *out       = ncc_alloc_array(ncc_parse_tree_ptr_t, 1);
        (*out)[0]  = build_result_tree(p, top_result);
        return;
    }

    pwz_exp_t *amb = find_top_ambiguity(top_result);

    if (amb) {
        size_t nalts = ncc_list_len(amb->alt.alts);

        *out_count = (int32_t)nalts;
        *out       = ncc_alloc_array(ncc_parse_tree_ptr_t, nalts);

        for (size_t i = 0; i < nalts; i++) {
            tree_convert_state_t st = {.pos = 0, .parser = p};
            (*out)[i] = convert_exp_to_tree(p, amb->alt.alts.data[i], &st);
        }

        return;
    }

    *out_count = 1;
    *out       = ncc_alloc_array(ncc_parse_tree_ptr_t, 1);
    (*out)[0]  = build_result_tree(p, top_result);
}

// ============================================================================
// Public API
// ============================================================================

ncc_pwz_parser_t *
ncc_pwz_new(ncc_grammar_t *g)
{
    ncc_grammar_finalize(g);

    ncc_pwz_parser_t *p = ncc_alloc(ncc_pwz_parser_t);

    p->grammar       = g;
    ncc_arena_init(&p->parse_arena, NCC_ARENA_DEFAULT_BLOCK_SIZE);
    p->all_exps      = ncc_list_new(pwz_exp_ptr_t);
    p->worklist      = ncc_list_new(pwz_zipper_t);
    p->worklist_swap = ncc_list_new(pwz_zipper_t);
    p->tops          = ncc_list_new(pwz_exp_ptr_t);

    // Sentinel nodes.
    p->exp_bottom       = ncc_alloc(pwz_exp_t);
    p->exp_bottom->kind = PWZ_SEQ;

    p->mem_bottom            = ncc_alloc(pwz_mem_t);
    p->mem_bottom->start_pos = PWZ_POS_BOTTOM;
    p->mem_bottom->end_pos   = PWZ_POS_BOTTOM;

    build_exp_graph(p, g);

    return p;
}

void
ncc_pwz_free(ncc_pwz_parser_t *p)
{
    if (!p) {
        return;
    }

    reset_memos(p);
    ncc_arena_free(&p->parse_arena);

    // Free grammar exp nodes (these are ncc_alloc'd, not arena-allocated).
    size_t num_exps = ncc_list_len(p->all_exps);

    for (size_t i = 0; i < num_exps; i++) {
        pwz_exp_t *e = p->all_exps.data[i];

        if (e->kind == PWZ_SEQ && e->seq.children) {
            ncc_free(e->seq.children);
        }
        else if (e->kind == PWZ_ALT) {
            ncc_list_free(e->alt.alts);
        }

        ncc_free(e);
    }

    ncc_list_free(p->all_exps);
    ncc_free(p->nt_exps);
    ncc_free(p->exp_bottom);
    ncc_free(p->mem_bottom);
    ncc_list_free(p->worklist);
    ncc_list_free(p->worklist_swap);
    ncc_list_free(p->tops);

    if (p->result_trees.data) {
        ncc_array_free(p->result_trees);
    }

    ncc_free(p);
}

void
ncc_pwz_release_parse_state(ncc_pwz_parser_t *p)
{
    if (!p) {
        return;
    }

    reset_memos(p);
    ncc_arena_free(&p->parse_arena);
    p->free_mems = nullptr;
    ncc_list_clear(p->worklist);
    ncc_list_clear(p->worklist_swap);
    ncc_list_clear(p->tops);
}

void
ncc_pwz_reset(ncc_pwz_parser_t *p)
{
    reset_memos(p);
    ncc_arena_reset(&p->parse_arena);
    p->free_mems = nullptr;
    ncc_list_clear(p->worklist);
    ncc_list_clear(p->worklist_swap);
    ncc_list_clear(p->tops);
    p->stream       = nullptr;
    p->result_tree  = nullptr;
    p->result_trees = (ncc_parse_tree_array_t){0};
}

bool
ncc_pwz_parse(ncc_pwz_parser_t   *p,
               ncc_token_stream_t *ts)
{
    ncc_pwz_reset(p);

    p->stream = ts;

    // Ensure the first token is available for init_parse.
    ncc_stream_get(ts, 0);

    init_parse(p);
    bool ok = run_parse(p);

    if (ok && ncc_list_len(p->tops) > 0) {
        p->result_tree = build_result_tree(p, p->tops.data[0]);
        return true;
    }

    return false;
}

ncc_parse_tree_t *
ncc_pwz_get_tree(ncc_pwz_parser_t *p)
{
    return p->result_tree;
}

ncc_parse_tree_array_t
ncc_pwz_get_trees(ncc_pwz_parser_t *p)
{
    if (p->result_trees.data) {
        return p->result_trees;
    }

    size_t ntops = ncc_list_len(p->tops);

    if (ntops == 0) {
        p->result_trees = ncc_array_new(ncc_parse_tree_ptr_t, 0);
        return p->result_trees;
    }

    if (ntops == 1) {
        ncc_parse_tree_ptr_t *raw_trees = nullptr;
        int32_t                raw_count = 0;

        enumerate_trees(p, p->tops.data[0], &raw_trees, &raw_count);

        p->result_trees = ncc_array_new(ncc_parse_tree_ptr_t, raw_count);

        for (int32_t i = 0; i < raw_count; i++) {
            ncc_array_set(p->result_trees, i, raw_trees[i]);
        }

        if (raw_trees) {
            ncc_free(raw_trees);
        }
    }
    else {
        p->result_trees = ncc_array_new(ncc_parse_tree_ptr_t, (int32_t)ntops);

        for (size_t i = 0; i < ntops; i++) {
            ncc_array_set(p->result_trees, (int32_t)i,
                           build_result_tree(p, p->tops.data[i]));
        }
    }

    return p->result_trees;
}

// ============================================================================
// Forest API
// ============================================================================

ncc_parse_forest_t
ncc_pwz_get_forest(ncc_pwz_parser_t *p)
{
    ncc_parse_tree_array_t trees = ncc_pwz_get_trees(p);

    return ncc_parse_forest_new(p->grammar, trees);
}

// ============================================================================
// One-shot parse (implements ncc_parse_fn_t)
// ============================================================================

ncc_parse_forest_t
ncc_pwz_parse_grammar(ncc_grammar_t      *g,
                        ncc_token_stream_t *ts)
{
    ncc_pwz_parser_t *p = ncc_pwz_new(g);
    bool               ok = ncc_pwz_parse(p, ts);

    if (!ok) {
        ncc_pwz_free(p);
        return ncc_parse_forest_empty(g);
    }

    ncc_parse_forest_t forest = ncc_pwz_get_forest(p);

    // Transfer tree ownership to caller; clear so ncc_pwz_free won't free them.
    p->result_trees = (ncc_parse_tree_array_t){0};
    ncc_pwz_free(p);

    return forest;
}
