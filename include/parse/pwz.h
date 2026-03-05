#pragma once

/**
 * @file pwz.h
 * @brief PWZ (Parsing With Zippers) derivative-based parser for arbitrary CFGs.
 *
 * Based on Darragh & Adams (ICFP 2020). Handles ambiguous and
 * left-recursive grammars via generalized zippers and memoization.
 */

#include "parse/grammar.h"
#include "parse/parse_forest.h"

typedef struct ncc_token_stream_t ncc_token_stream_t;

// ============================================================================
// Opaque parser state
// ============================================================================

/** @brief Opaque PWZ parser state. */
typedef struct ncc_pwz_parser_t ncc_pwz_parser_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new PWZ parser for a grammar.
 *
 * Finalizes the grammar if not already done, then builds the
 * internal expression graph.
 *
 * @param g  Grammar to parse with (must not be nullptr).
 * @return Newly allocated PWZ parser.
 */
ncc_pwz_parser_t *ncc_pwz_new(ncc_grammar_t *g);

/**
 * @brief Free a PWZ parser and all associated state.
 * @param p  PWZ parser to free (nullptr is a no-op).
 */
void ncc_pwz_free(ncc_pwz_parser_t *p);

/**
 * @brief Reset a PWZ parser for reuse with the same grammar.
 *
 * Clears per-parse state (memos, worklists, results) but keeps the
 * expression graph intact.
 *
 * @param p  PWZ parser to reset.
 */
void ncc_pwz_reset(ncc_pwz_parser_t *p);

// ============================================================================
// Parsing
// ============================================================================

/**
 * @brief Parse from a token stream with the PWZ parser.
 *
 * Tokens are consumed lazily from the stream as needed.
 *
 * @param p   PWZ parser to use.
 * @param ts  Token stream to consume.
 * @return True if parsing succeeded (at least one complete parse found).
 */
bool ncc_pwz_parse(ncc_pwz_parser_t   *p,
                     ncc_token_stream_t *ts);

// ============================================================================
// Results
// ============================================================================

/**
 * @brief Get the parse tree from the last successful parse.
 *
 * Returns the single "best" tree (longest / latest completion).
 *
 * @param p  PWZ parser to query.
 * @return The parse tree, or nullptr if the last parse failed.
 */
ncc_parse_tree_t *ncc_pwz_get_tree(ncc_pwz_parser_t *p);

/**
 * @brief Get all parse trees from an ambiguous parse.
 * @param p  PWZ parser to query.
 * @return Array of parse tree pointers (empty if last parse failed).
 */
ncc_parse_tree_array_t ncc_pwz_get_trees(ncc_pwz_parser_t *p);

/**
 * @brief Get a parse forest from the last successful parse.
 * @param p  PWZ parser to query.
 * @return Forest wrapping all trees (empty if last parse failed).
 */
ncc_parse_forest_t ncc_pwz_get_forest(ncc_pwz_parser_t *p);

// ============================================================================
// One-shot parse (implements ncc_parse_fn_t)
// ============================================================================

/**
 * @brief Parse a token stream against a grammar using PWZ in a single call.
 *
 * Wraps the full PWZ lifecycle (new → parse → get_forest → free).
 * Signature matches `ncc_parse_fn_t` so it can be used as a
 * pluggable parse engine.
 *
 * @param g   Grammar to parse with.
 * @param ts  Token stream to consume.
 * @return Parse forest (empty on failure).
 */
ncc_parse_forest_t ncc_pwz_parse_grammar(ncc_grammar_t      *g,
                                             ncc_token_stream_t *ts);
