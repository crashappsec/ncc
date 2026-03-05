#pragma once

/**
 * @file token_stream.h
 * @brief Streaming token iteration with peek/rewind over a growable list.
 *
 * The token stream wraps a scanner and provides forward iteration,
 * lookahead (`peek`), lookback (`lookback`), rewind, save/restore,
 * random access by position, and reset (re-tokenize from scratch)
 * for speculative parsing — all over a position-indexed growable list.
 *
 * ### Usage
 *
 * ```c
 * ncc_token_stream_t *ts = ncc_token_stream_new(scanner);
 * ncc_stream_foreach(ts, tok) {
 *     printf("[%d] %s\n", tok->tid, tok->value);
 * }
 * ncc_token_stream_free(ts);
 * ```
 *
 * ### Related modules
 *
 * - `parsers/scanner.h` — scanner (token producer)
 * - `slay/token.h` — token types
 */

#include "scanner/scanner.h"

// ============================================================================
// Token stream struct
// ============================================================================

struct ncc_token_stream_t {
    ncc_scanner_t    *scanner;       /**< Produces tokens on demand. */

    // Token storage (growable, position-indexed)
    ncc_token_info_t **tokens;       /**< Pointer array of all tokens. */
    int32_t             token_cap;    /**< Allocated capacity. */
    int32_t             token_count;  /**< Number of tokens stored. */

    // Read position
    int32_t            pos;           /**< Current read cursor (absolute). */

    // Token indexing
    int32_t            next_index;    /**< Global token index counter. */

    // State
    bool               exhausted;     /**< True when scanner done + all consumed. */
};

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a token stream wrapping a scanner.
 *
 * @param scanner The scanner that produces tokens.
 */
extern ncc_token_stream_t *
ncc_token_stream_new(ncc_scanner_t *scanner);

/**
 * @brief Free a token stream (does not free the scanner).
 * @param ts Token stream to free.
 */
extern void ncc_token_stream_free(ncc_token_stream_t *ts);

// ============================================================================
// Forward iteration
// ============================================================================

/**
 * @brief Get the next token and advance the position.
 *
 * Returns nullptr when the stream is exhausted.
 * The returned pointer remains valid for the lifetime of the stream.
 */
extern ncc_token_info_t *ncc_stream_next(ncc_token_stream_t *ts);

/** @brief True if the stream has no more tokens. */
extern bool ncc_stream_is_done(ncc_token_stream_t *ts);

// ============================================================================
// Peek (lookahead)
// ============================================================================

/**
 * @brief Peek at a token N positions ahead of current without advancing.
 *
 * `ncc_stream_peek(ts, 0)` returns the token that `next()` would return.
 * Returns nullptr if N exceeds available tokens.
 */
extern ncc_token_info_t *ncc_stream_peek(ncc_token_stream_t *ts, int32_t n);

// ============================================================================
// Rewind (lookback)
// ============================================================================

/**
 * @brief Move the read position back by N tokens.
 *
 * @return true on success, false if N exceeds available history.
 */
extern bool ncc_stream_rewind(ncc_token_stream_t *ts, int32_t n);

/**
 * @brief Peek at a token N positions behind current (already consumed).
 *
 * `ncc_stream_lookback(ts, 1)` returns the most recently consumed token.
 * Returns nullptr if N exceeds available history.
 */
extern ncc_token_info_t *ncc_stream_lookback(ncc_token_stream_t *ts,
                                                int32_t n);

// ============================================================================
// Random access
// ============================================================================

/**
 * @brief Get token at absolute position, triggering lazy tokenization if needed.
 *
 * Returns nullptr if the position is beyond EOF.
 */
extern ncc_token_info_t *ncc_stream_get(ncc_token_stream_t *ts, int32_t pos);

/**
 * @brief Total number of tokens stored so far.
 */
extern int32_t ncc_stream_token_count(ncc_token_stream_t *ts);

// ============================================================================
// Reset (re-tokenize from scratch)
// ============================================================================

/**
 * @brief Reset the stream: free all tokens, reset scanner, re-tokenize fresh.
 *
 * After reset the stream is at position 0 with no tokens cached.
 * This undoes any in-place token mutations (e.g., reclassify).
 */
extern void ncc_stream_reset(ncc_token_stream_t *ts);

// ============================================================================
// Save / restore (speculative parsing)
// ============================================================================

typedef struct {
    int32_t pos;  /**< Saved read position. */
} ncc_stream_mark_t;

/** @brief Save the current stream position. */
extern ncc_stream_mark_t ncc_stream_save(ncc_token_stream_t *ts);

/**
 * @brief Restore to a previously saved position.
 * @return false if the saved position is beyond current token count.
 */
extern bool ncc_stream_restore(ncc_token_stream_t *ts,
                                 ncc_stream_mark_t mark);

/** @brief Discard a save point (no-op, for symmetry / readability). */
static inline void
ncc_stream_commit(ncc_stream_mark_t mark)
{
    (void)mark;
}

// ============================================================================
// Collect all (drain to token list)
// ============================================================================

/**
 * @brief Drain the entire stream into an `ncc_list_t(ncc_token_info_t)`.
 *
 * Useful for parsers that need all tokens up front (e.g., Earley).
 * After this call the stream is exhausted.
 */
extern ncc_list_t(ncc_token_info_t) ncc_stream_collect(ncc_token_stream_t *ts);

// ============================================================================
// Array-backed stream (for tests and pre-tokenized input)
// ============================================================================

/**
 * @brief Create a token stream backed by a pre-built array of token pointers.
 *
 * The returned stream yields each token from the array in order.
 * The token pointers are borrowed; the caller must keep them alive
 * for the lifetime of the stream.
 *
 * @param tokens Array of token pointers.
 * @param count  Number of tokens in the array.
 * @return A new token stream.
 */
extern ncc_token_stream_t *
ncc_token_stream_from_array(ncc_token_info_t **tokens, int32_t count);

// ============================================================================
// Codepoint-level stream (for character-level grammars)
// ============================================================================

/**
 * @brief Create a token stream where each codepoint is its own token.
 *
 * Iterates over the UTF-8 bytes of @p input, producing one token per
 * codepoint.  Each token's `tid` is set to the codepoint value (so
 * the parser can match against `NCC_CHAR('x')` or `NCC_CLASS(...)` items).
 * A trailing EOF token is appended.
 *
 * @param input  The string to tokenize codepoint-by-codepoint.
 * @return A new token stream.
 */
extern ncc_token_stream_t *
ncc_token_stream_from_codepoints(ncc_string_t input);

// ============================================================================
// Iteration macro
// ============================================================================

/**
 * @brief Iterate over all tokens in a stream.
 *
 * Usage:
 * ```c
 * ncc_stream_foreach(ts, tok) {
 *     printf("token: %s\n", tok->value);
 * }
 * ```
 */
#define ncc_stream_foreach(ts, tok_var)                       \
    for (ncc_token_info_t *(tok_var) = ncc_stream_next(ts);  \
         (tok_var) != nullptr;                                    \
         (tok_var) = ncc_stream_next(ts))
