// parse_forest.c - Engine-agnostic parse forest operations.

#include "parse/parse_forest.h"
#include "lib/array.h"

// ============================================================================
// Construction
// ============================================================================

ncc_parse_forest_t
ncc_parse_forest_empty(ncc_grammar_t *g)
{
    return (ncc_parse_forest_t){
        .trees   = ncc_array_new(ncc_parse_tree_ptr_t, 0),
        .grammar = g,
    };
}

ncc_parse_forest_t
ncc_parse_forest_new(ncc_grammar_t          *g,
                       ncc_parse_tree_array_t  trees)
{
    return (ncc_parse_forest_t){
        .trees   = trees,
        .grammar = g,
    };
}

// ============================================================================
// Query
// ============================================================================

int32_t
ncc_parse_forest_count(ncc_parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return 0;
    }

    return (int32_t)f->trees.len;
}

bool
ncc_parse_forest_is_ambiguous(ncc_parse_forest_t *f)
{
    return ncc_parse_forest_count(f) > 1;
}

// ============================================================================
// Access
// ============================================================================

ncc_parse_tree_t *
ncc_parse_forest_tree(ncc_parse_forest_t *f, int32_t ix)
{
    if (!f || !f->trees.data || ix < 0 || (size_t)ix >= f->trees.len) {
        return nullptr;
    }

    return f->trees.data[ix];
}

ncc_parse_tree_t *
ncc_parse_forest_best(ncc_parse_forest_t *f)
{
    return ncc_parse_forest_tree(f, 0);
}

// ============================================================================
// Walk
// ============================================================================

void *
ncc_parse_forest_walk(ncc_parse_forest_t *f,
                        int32_t              ix,
                        void                *thunk)
{
    ncc_parse_tree_t *tree = ncc_parse_forest_tree(f, ix);

    if (!tree || !f->grammar) {
        return nullptr;
    }

    return ncc_parse_tree_walk(f->grammar, tree, thunk);
}

void *
ncc_parse_forest_walk_best(ncc_parse_forest_t *f,
                             void               *thunk)
{
    return ncc_parse_forest_walk(f, 0, thunk);
}

// ============================================================================
// Cleanup
// ============================================================================

void
ncc_parse_forest_free(ncc_parse_forest_t *f)
{
    if (!f || !f->trees.data) {
        return;
    }

    for (size_t i = 0; i < f->trees.len; i++) {
        ncc_parse_tree_free(f->trees.data[i]);
    }

    ncc_array_free(f->trees);
    f->trees = (ncc_parse_tree_array_t){0};
}
