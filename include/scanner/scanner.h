#pragma once

/**
 * @file scanner.h
 * @brief Streaming tokenizer scanner — implementor-facing layer.
 *
 * A tokenizer author writes a callback that receives a scanner, uses
 * helpers to consume input bytes, and calls `ncc_scan_emit()` to
 * produce tokens.  The scanner handles position tracking, mark/extract,
 * string ownership, and trivia collection.
 *
 * ### Usage
 *
 * ```c
 * static bool my_tokenizer(ncc_scanner_t *s) {
 *     ncc_scan_skip_whitespace(s);
 *     if (ncc_scan_at_eof(s)) return false;
 *     ncc_scan_mark(s);
 *     ncc_scan_advance(s);
 *     ncc_scan_emit_marked(s, MY_TOK);
 *     return true;
 * }
 * ```
 *
 * ### Related modules
 *
 * - `parsers/token_stream.h` — consumer-facing token iteration
 * - `parsers/scan_recipes.h` — higher-level scanning helpers
 * - `slay/token.h` — token types
 * - `core/buffer.h` — input buffer type
 */

#include "ncc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/dict.h"
#include "lib/result.h"
#include "parse/token.h"
#include "parse/types.h"
#include "unicode/encoding.h"
#include "unicode/identifiers.h"
#include "unicode/query.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct ncc_scanner_t      ncc_scanner_t;
typedef struct ncc_token_stream_t ncc_token_stream_t;

// ============================================================================
// Error codes for scanner operations
// ============================================================================

/** @brief The needle string was not found before EOF. */
#define NCC_ERR_SCAN_NOT_FOUND  (-10)

// (Owned string list removed — ncc_string_t data is GC-managed.)

// ============================================================================
// Callback typedef
// ============================================================================

/**
 * @brief Tokenizer callback — called repeatedly to produce one token.
 *
 * Return `true` to indicate a token was emitted (or skip/trivia handled).
 * Return `false` to indicate end-of-input (no more tokens).
 *
 * The callback uses scanner helpers (`ncc_scan_peek`, `ncc_scan_advance`,
 * etc.) to inspect and consume input, then calls `ncc_scan_emit()` to
 * produce a token.
 */
typedef bool (*ncc_scan_cb_t)(ncc_scanner_t *s);

/**
 * @brief Optional callback invoked by `ncc_scanner_reset()` to reset
 *        user state (e.g., lexer modes, array scan position).
 */
typedef void (*ncc_scan_reset_cb_t)(ncc_scanner_t *s);

// ============================================================================
// Scanner struct
// ============================================================================

struct ncc_scanner_t {
    // Input
    const char       *input;       /**< Raw input bytes (from buffer). */
    size_t            input_len;   /**< Total input byte length. */
    size_t            cursor;      /**< Current byte offset into input. */

    // Position tracking
    uint32_t                       line;   /**< Current 1-based line number. */
    uint32_t                       column; /**< Current 1-based column number. */
    ncc_option_t(ncc_string_t)   file;   /**< Source filename (optional). */

    // Token mark (start of current token)
    size_t            mark;        /**< Byte offset where current token started. */
    uint32_t          mark_line;   /**< Line at mark. */
    uint32_t          mark_col;    /**< Column at mark. */

    // Callback + state
    ncc_scan_cb_t        cb;          /**< User's tokenizer callback. */
    ncc_scan_reset_cb_t  reset_cb;    /**< Optional callback to reset user state. */
    void                 *user_state;  /**< Opaque user state (lexer modes, etc.). */

    // Grammar linkage (optional)
    ncc_grammar_t   *grammar;     /**< If set, used for terminal ID lookups. */

    // Output target (the token stream fills this)
    ncc_token_stream_t *stream;   /**< Back-pointer to owning stream. */

    // Terminal ID cache (for grammar integration)
    ncc_dict_t *terminal_ids; /**< Lazily built name→id cache from grammar. */

    // Trivia collection
    ncc_trivia_t    *pending_leading;  /**< Leading trivia for next token. */
    ncc_trivia_t    *pending_trailing; /**< Trailing trivia for prev token. */

    bool              at_eof;      /**< True after callback returns false. */
};

// ============================================================================
// Scanner lifecycle
// ============================================================================

/**
 * @brief Create a scanner over a buffer.
 *
 * @param buf     Input buffer (bytes are borrowed, not copied).
 * @param cb      Tokenizer callback.
 * @param grammar Optional grammar for terminal ID resolution (may be nullptr).
 *
 * @kw file  Source filename for diagnostics (default: nullptr).
 * @kw state Initial user state pointer (default: nullptr).
 */
extern ncc_scanner_t *
ncc_scanner_new(ncc_buffer_t                *buf,
                 ncc_scan_cb_t                cb,
                 ncc_grammar_t               *grammar,
                 ncc_option_t(ncc_string_t)  file,
                 void                         *state,
                 ncc_scan_reset_cb_t          reset_cb);

/**
 * @brief Free a scanner.
 * @param s Scanner to free.
 */
extern void ncc_scanner_free(ncc_scanner_t *s);

/**
 * @brief Reset a scanner to the beginning of its input.
 *
 * Resets cursor, line/column, mark, pending trivia, and EOF flag.
 * If `reset_cb` is set, calls it to reset user state.
 */
extern void ncc_scanner_reset(ncc_scanner_t *s);

// ============================================================================
// Cursor inspection
// ============================================================================

/**
 * @brief Peek at the codepoint at the given byte offset from cursor.
 *
 * @param s      Scanner.
 * @param offset Byte offset from cursor (0 = current position).
 * @return Decoded codepoint, or 0 at EOF or if offset underflows.
 */
extern ncc_codepoint_t ncc_scan_peek(ncc_scanner_t *s, int32_t offset);

/**
 * @brief Peek at raw byte at offset from cursor.
 * @param s      Scanner.
 * @param offset Byte offset from cursor.
 * @return Byte value, or 0 at EOF or if offset underflows.
 */
extern uint8_t ncc_scan_peek_byte(ncc_scanner_t *s, int32_t offset);

/** @brief True if cursor is at end of input. */
extern bool ncc_scan_at_eof(ncc_scanner_t *s);

/** @brief Remaining bytes from cursor to end. */
extern size_t ncc_scan_remaining(ncc_scanner_t *s);

/** @brief Current byte offset. */
extern size_t ncc_scan_offset(ncc_scanner_t *s);

// ============================================================================
// Cursor movement
// ============================================================================

/** @brief Advance cursor by one codepoint, updating line/column. */
extern void ncc_scan_advance(ncc_scanner_t *s);

/** @brief Advance cursor by N codepoints. */
extern void ncc_scan_advance_n(ncc_scanner_t *s, int32_t n);

/** @brief Advance cursor by N raw bytes (for binary protocols). */
extern void ncc_scan_advance_bytes(ncc_scanner_t *s, size_t n);

// ============================================================================
// Matching helpers
// ============================================================================

/**
 * @brief Match exact string literal.
 * @return Byte length matched, or 0 if no match.
 * @post Cursor is advanced past the match on success.
 */
extern size_t ncc_scan_match_str(ncc_scanner_t *s, const char *lit);

/**
 * @brief Match one codepoint satisfying predicate.
 * @return UTF-8 byte length of matched codepoint, or 0.
 * @post Cursor is advanced past the match on success.
 */
extern size_t ncc_scan_match_if(ncc_scanner_t *s, ncc_cp_predicate_fn pred);

/**
 * @brief Match one codepoint in the given character class.
 * @return Byte length of matched codepoint, or 0.
 * @post Cursor is advanced past the match on success.
 */
extern size_t ncc_scan_match_class(ncc_scanner_t *s, ncc_char_class_t cc);

/**
 * @brief Match one specific codepoint.
 * @return Byte length of matched codepoint, or 0.
 * @post Cursor is advanced past the match on success.
 */
extern size_t ncc_scan_match_cp(ncc_scanner_t *s, ncc_codepoint_t cp);

/**
 * @brief Skip zero or more codepoints matching predicate.
 * @return Count of codepoints skipped.
 */
extern int32_t ncc_scan_skip_while(ncc_scanner_t *s, ncc_cp_predicate_fn pred);

/**
 * @brief Skip zero or more codepoints in character class.
 * @return Count of codepoints skipped.
 */
extern int32_t ncc_scan_skip_class(ncc_scanner_t *s, ncc_char_class_t cc);

/**
 * @brief Skip until (but not past) the stop codepoint.
 * @return Bytes skipped.
 */
extern size_t ncc_scan_skip_until_cp(ncc_scanner_t *s, ncc_codepoint_t stop);

/**
 * @brief Skip until (but not past) the given string.
 *
 * @return `ncc_result_t(size_t)` — ok with bytes skipped if needle was found,
 *         or err with `NCC_ERR_SCAN_NOT_FOUND` if EOF was reached without
 *         finding the needle.
 *
 * @post On success, cursor is positioned at the start of the needle.
 *       On failure, cursor is at EOF.
 */
extern ncc_result_t(size_t)
ncc_scan_skip_until_str(ncc_scanner_t *s, const char *needle);

// ============================================================================
// Mark / extract
// ============================================================================

/** @brief Set the mark to the current cursor position. */
extern void ncc_scan_mark(ncc_scanner_t *s);

/**
 * @brief Extract text from mark to cursor as an `ncc_string_t`.
 *
 * The backing data is GC-managed.
 */
extern ncc_string_t ncc_scan_extract(ncc_scanner_t *s);

/** @brief Length in bytes from mark to cursor. */
extern size_t ncc_scan_mark_len(ncc_scanner_t *s);

// ============================================================================
// Token emission
// ============================================================================

/**
 * @brief Emit a token into the stream.
 *
 * Uses mark position for start line/column, cursor position for end column.
 * After emit, the mark is automatically reset to the current cursor position.
 *
 * @param s     Scanner.
 * @param tid   Terminal ID for the token.
 * @param value Token text, or `ncc_option_none(ncc_string_t)`.
 */
extern void ncc_scan_emit(ncc_scanner_t *s, int32_t tid,
                            ncc_option_t(ncc_string_t) value);

/**
 * @brief Emit using the text from mark to cursor as the value.
 *
 * Equivalent to: `ncc_scan_emit(s, tid, ncc_scan_extract(s))`.
 */
extern void ncc_scan_emit_marked(ncc_scanner_t *s, int32_t tid);

// ============================================================================
// Trivia helpers
// ============================================================================

/** @brief Collect text as leading trivia for the next token. */
extern void ncc_scan_add_leading_trivia(ncc_scanner_t *s,
                                          ncc_string_t text);

/** @brief Collect trailing trivia for the most recently emitted token. */
extern void ncc_scan_add_trailing_trivia(ncc_scanner_t *s,
                                           ncc_string_t text);

/**
 * @brief Skip whitespace, collecting it as leading trivia.
 * @return Count of codepoints skipped.
 */
extern int32_t ncc_scan_skip_whitespace(ncc_scanner_t *s);

/**
 * @brief Skip a line comment (to newline), collecting as trailing trivia.
 * @pre Cursor is positioned at the start of the comment text.
 */
extern void ncc_scan_skip_line_comment(ncc_scanner_t *s);

/**
 * @brief Skip a block comment, collecting as leading trivia.
 *
 * @param s      Scanner.
 * @param opener Opening delimiter (e.g. "/ *").
 * @param closer Closing delimiter (e.g. "* /").
 * @return true if closer was found; false if unterminated.
 *
 * @pre Cursor is positioned at the start of the opening delimiter.
 */
extern bool ncc_scan_skip_block_comment(ncc_scanner_t *s,
                                          const char *opener,
                                          const char *closer);

// ============================================================================
// Grammar integration
// ============================================================================

/**
 * @brief Look up a terminal ID by name in the scanner's grammar.
 * @return The terminal ID, or `NCC_TOK_OTHER` if not found or no grammar.
 */
extern int64_t ncc_scan_terminal_id(ncc_scanner_t *s, const char *name);
