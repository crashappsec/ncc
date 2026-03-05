#pragma once

/**
 * @file unicode_class.h
 * @internal
 * @brief Character class matching for parser engines.
 *
 * Dispatches `ncc_char_class_t` values against n00b's unicode
 * property tables (`unicode/identifiers.h`, `unicode/properties.h`).
 */

#include "parse/types.h"
#include "unicode/identifiers.h"
#include "unicode/properties.h"

/**
 * @brief Test if a codepoint matches a character class.
 * @param cp  Unicode codepoint.
 * @param cc  Character class to test against.
 * @return True if the codepoint belongs to the class.
 */
bool ncc_codepoint_matches_class(int32_t cp, ncc_char_class_t cc);
