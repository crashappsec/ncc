#pragma once

/**
 * @file scan_recipes.h
 * @brief Higher-level scanning recipes for common token patterns.
 *
 * Tokenizer callbacks compose these to avoid reimplementing string
 * escapes, number literals, identifiers, etc.
 *
 * All recipes operate on the scanner's cursor and return
 * `ncc_option_t(ncc_string_t)`.  On failure (no match or unterminated
 * literal) they return `ncc_option_none(ncc_string_t)` and leave the
 * cursor unchanged.
 *
 * ### Related modules
 *
 * - `parsers/scanner.h` — scanner API used by recipes
 */

#include "scanner/scanner.h"

// ============================================================================
// String recipes
// ============================================================================

/**
 * @brief Scan a double-quoted string literal with standard escape sequences.
 *
 * Handles: `\\`, `\n`, `\t`, `\r`, `\0`, `\"`, `\xHH`, `\uHHHH`,
 * `\UHHHHHHHH`.  Advances cursor past closing quote.  Sets mark before
 * opening quote.
 *
 * @return Decoded string value, or none on unterminated string or if
 *         cursor is not at `"`.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_string_double(ncc_scanner_t *s);

/**
 * @brief Scan a single-quoted string/char literal.
 *
 * Same escape handling as `ncc_scan_string_double`.
 *
 * @return Decoded string value, or none on error.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_string_single(ncc_scanner_t *s);

/**
 * @brief Scan a raw/verbatim string (no escape processing).
 *
 * @param s     Scanner.
 * @param quote The quote character (or string) that opens/closes.
 * @return Raw string value, or none on error.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_string_raw(ncc_scanner_t *s, const char *quote);

// ============================================================================
// Number recipes
// ============================================================================

/**
 * @brief Scan an integer literal (decimal, hex 0x, octal 0o, binary 0b).
 *
 * Handles optional `_` digit separators.  Does NOT handle sign (+/-).
 * Advances cursor past the last digit.
 *
 * @return Extracted text, or none if no digits found.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_integer(ncc_scanner_t *s);

/**
 * @brief Scan a floating-point literal (with optional exponent).
 *
 * Matches: `digits ['.' digits] [('e'|'E') ['+'/'-'] digits]`
 * Handles `_` digit separators.
 *
 * @return Extracted text, or none if no match.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_float(ncc_scanner_t *s);

/**
 * @brief Scan a number (integer or float) and emit appropriate token.
 *
 * @param s         Scanner.
 * @param int_tid   Terminal ID to emit for integers.
 * @param float_tid Terminal ID to emit for floats.
 * @return true if a number was scanned and emitted.
 */
extern bool ncc_scan_number(ncc_scanner_t *s,
                              int32_t int_tid,
                              int32_t float_tid);

// ============================================================================
// Identifier recipe
// ============================================================================

/**
 * @brief Scan an identifier (ID_Start followed by ID_Continue*).
 *
 * Uses Unicode UAX#31 character properties.
 *
 * @return Extracted text, or none if no ID_Start at cursor.
 */
extern ncc_option_t(ncc_string_t) ncc_scan_identifier(ncc_scanner_t *s);
