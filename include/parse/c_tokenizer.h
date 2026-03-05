#pragma once

/**
 * @file c_tokenizer.h
 * @brief C/ncc tokenizer callback for the scanner framework.
 *
 * Provides `ncc_c_tokenize`, a `ncc_scan_cb_t` callback that tokenizes
 * C23 + ncc extension source code. Handles:
 *
 * - All C23 keywords and types
 * - GCC/clang extension keywords
 * - ncc extension keywords (typeid, _kargs, once, etc.)
 * - String/char literals with encoding prefixes (L, u, U, u8)
 * - Integer, float, hex, octal, binary literals with suffixes
 * - All C operators (including multi-character)
 * - Line comments, block comments
 * - Preprocessor directives (skipped as trivia)
 * - `#pragma ncc off/on` tracking
 */

#include "scanner/scanner.h"
#include "lib/string.h"

// ============================================================================
// Tokenizer state
// ============================================================================

typedef struct {
    ncc_string_t current_file;
    bool         ncc_off;
    bool         in_system_header;
} ncc_c_tokenizer_state_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Scanner callback for C/ncc source code.
 * @param s  Scanner with input positioned at next token.
 * @return true if a token was emitted, false at EOF.
 */
bool ncc_c_tokenize(ncc_scanner_t *s);

/**
 * @brief Reset callback for the C tokenizer.
 */
void ncc_c_tokenizer_reset(ncc_scanner_t *s);

/**
 * @brief Allocate and initialize a C tokenizer state.
 */
ncc_c_tokenizer_state_t *ncc_c_tokenizer_state_new(void);

/**
 * @brief Check whether the tokenizer is in an `ncc off` region.
 */
bool ncc_c_tokenizer_is_ncc_off(ncc_scanner_t *s);
