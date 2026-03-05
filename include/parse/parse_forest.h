#pragma once

/**
 * @file parse_forest.h
 * @brief Engine-agnostic parse forest — standard result type for all parsers.
 *
 * Wraps the raw tree array with metadata and convenience accessors.
 * Produced by all parser engines (PWZ, Earley, packrat).
 */

#include "parse/parse_tree.h"

typedef struct ncc_token_stream_t ncc_token_stream_t;

// ============================================================================
// Parse forest type
// ============================================================================

/**
 * @brief A parse forest: zero or more parse trees from an ambiguous parse.
 *
 * Returned by value from parser engines.  An empty forest (`count == 0`)
 * indicates parse failure.
 */
typedef struct ncc_parse_forest_t {
    ncc_parse_tree_array_t  trees;   /**< The parse trees (may be empty). */
    ncc_grammar_t          *grammar; /**< Grammar used (for tree walking). */
} ncc_parse_forest_t;

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Create an empty forest (parse failure).
 * @param g  Grammar that was used.
 */
ncc_parse_forest_t ncc_parse_forest_empty(ncc_grammar_t *g);

/**
 * @brief Wrap existing trees in a forest.
 * @param g      Grammar that was used.
 * @param trees  Array of parse tree pointers.
 */
ncc_parse_forest_t ncc_parse_forest_new(ncc_grammar_t          *g,
                                           ncc_parse_tree_array_t  trees);

// ============================================================================
// Query
// ============================================================================

/** @brief Number of trees in the forest. */
int32_t ncc_parse_forest_count(ncc_parse_forest_t *f);

/** @brief True if the forest contains more than one tree. */
bool ncc_parse_forest_is_ambiguous(ncc_parse_forest_t *f);

// ============================================================================
// Access
// ============================================================================

/**
 * @brief Get tree at index.
 * @param f   Forest to query.
 * @param ix  Zero-based index.
 * @return Tree pointer, or nullptr if out of bounds.
 */
ncc_parse_tree_t *ncc_parse_forest_tree(ncc_parse_forest_t *f, int32_t ix);

/**
 * @brief Get the best (first) tree.
 * @return Tree pointer, or nullptr if the forest is empty.
 */
ncc_parse_tree_t *ncc_parse_forest_best(ncc_parse_forest_t *f);

// ============================================================================
// Walk
// ============================================================================

/**
 * @brief Walk tree at index using grammar-registered actions.
 * @param f      Forest.
 * @param ix     Tree index.
 * @param thunk  User context passed to walk actions.
 * @return Result from the root walk action.
 */
void *ncc_parse_forest_walk(ncc_parse_forest_t *f,
                              int32_t              ix,
                              void                *thunk);

/**
 * @brief Walk the best (first) tree.
 */
void *ncc_parse_forest_walk_best(ncc_parse_forest_t *f,
                                   void               *thunk);

// ============================================================================
// Cleanup
// ============================================================================

/**
 * @brief Free all trees in the forest.
 *
 * Frees tree node structures but not the tokens themselves
 * (tokens are owned by the token list / parser).
 */
void ncc_parse_forest_free(ncc_parse_forest_t *f);

// ============================================================================
// Updated parser engine abstraction
// ============================================================================

/**
 * @brief Generic parse function signature for any parser engine.
 *
 * Each engine (PWZ, Earley, packrat) provides one function matching
 * this signature.  Returns a forest by value — empty forest means
 * parse failure.
 *
 * @param g   Grammar to parse with.
 * @param ts  Token stream to consume.
 * @return Parse forest (empty on failure).
 */
typedef ncc_parse_forest_t (*ncc_parse_fn_t)(ncc_grammar_t      *g,
                                                ncc_token_stream_t *ts);
