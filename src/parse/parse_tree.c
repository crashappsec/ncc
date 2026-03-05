// parse_tree.c - Shared parse tree operations (engine-agnostic).

#include "parse/parse_tree.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"

// ============================================================================
// Node inspection
// ============================================================================

bool
ncc_parse_node_is_token(ncc_parse_tree_t *t)
{
    return t && ncc_tree_is_leaf(t);
}

ncc_token_info_t *
ncc_parse_node_token(ncc_parse_tree_t *t)
{
    if (!t || !ncc_tree_is_leaf(t)) {
        return nullptr;
    }

    return ncc_tree_leaf_value(t);
}

ncc_option_t(ncc_string_t)
ncc_parse_node_name(ncc_parse_tree_t *t)
{
    if (!t || ncc_tree_is_leaf(t)) {
        return ncc_option_none(ncc_string_t);
    }

    return ncc_option_set(ncc_string_t, ncc_tree_node_value(t).name);
}

// ============================================================================
// Tree walking
// ============================================================================

void *
ncc_parse_tree_walk(ncc_grammar_t *g, ncc_parse_tree_t *node, void *thunk)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_tree_leaf_value(node);
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);

    if (pn->id == NCC_EMPTY_STRING) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);

    void **sub_results = nullptr;

    if (nc) {
        sub_results = ncc_alloc_array(void *, nc);

        for (size_t i = 0; i < nc; i++) {
            sub_results[i] = ncc_parse_tree_walk(g,
                                                   ncc_tree_child(node, i),
                                                   thunk);
        }
    }

    // Group wrapper nodes (from BNF ?, *, +) have pn->id = NCC_GROUP_ID
    // which won't resolve to a real nonterm. Skip the action lookup.
    if (pn->group_top) {
        return sub_results;
    }

    ncc_nonterm_t    *nt     = ncc_get_nonterm(g, pn->id);
    ncc_walk_action_t action = (nt && nt->action) ? nt->action
                                                    : g->default_action;

    if (!action) {
        return sub_results;
    }

    return (*action)(pn, sub_results, thunk);
}

// ============================================================================
// Tree free
// ============================================================================

void
ncc_parse_tree_free(ncc_parse_tree_t *t)
{
    if (!t) {
        return;
    }

    if (!t->is_leaf) {
        for (size_t i = 0; i < t->node.num_children; i++) {
            ncc_parse_tree_free(t->node.children[i]);
        }
    }

    ncc_tree_free_node(t);
}
