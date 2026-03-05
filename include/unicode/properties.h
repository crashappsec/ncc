#pragma once
/**
 * @file properties.h
 * @brief Unicode property types and ASCII-only stubs (standalone extraction).
 *
 * Provides the type definitions (enums) from the full n00b Unicode library
 * and stub implementations that handle ASCII only. The parser does not
 * need full Unicode property lookup -- just the types for compilation.
 */

#include "ncc.h"
#include "lib/option.h"

// ============================================================================
// General_Category
// ============================================================================

typedef enum {
    NCC_UNICODE_GC_LU, /**< Letter, uppercase */
    NCC_UNICODE_GC_LL, /**< Letter, lowercase */
    NCC_UNICODE_GC_LT, /**< Letter, titlecase */
    NCC_UNICODE_GC_LM, /**< Letter, modifier */
    NCC_UNICODE_GC_LO, /**< Letter, other */
    NCC_UNICODE_GC_MN, /**< Mark, nonspacing */
    NCC_UNICODE_GC_MC, /**< Mark, spacing combining */
    NCC_UNICODE_GC_ME, /**< Mark, enclosing */
    NCC_UNICODE_GC_ND, /**< Number, decimal digit */
    NCC_UNICODE_GC_NL, /**< Number, letter */
    NCC_UNICODE_GC_NO, /**< Number, other */
    NCC_UNICODE_GC_PC, /**< Punctuation, connector */
    NCC_UNICODE_GC_PD, /**< Punctuation, dash */
    NCC_UNICODE_GC_PS, /**< Punctuation, open */
    NCC_UNICODE_GC_PE, /**< Punctuation, close */
    NCC_UNICODE_GC_PI, /**< Punctuation, initial quote */
    NCC_UNICODE_GC_PF, /**< Punctuation, final quote */
    NCC_UNICODE_GC_PO, /**< Punctuation, other */
    NCC_UNICODE_GC_SM, /**< Symbol, math */
    NCC_UNICODE_GC_SC, /**< Symbol, currency */
    NCC_UNICODE_GC_SK, /**< Symbol, modifier */
    NCC_UNICODE_GC_SO, /**< Symbol, other */
    NCC_UNICODE_GC_ZS, /**< Separator, space */
    NCC_UNICODE_GC_ZL, /**< Separator, line */
    NCC_UNICODE_GC_ZP, /**< Separator, paragraph */
    NCC_UNICODE_GC_CC, /**< Other, control */
    NCC_UNICODE_GC_CF, /**< Other, format */
    NCC_UNICODE_GC_CS, /**< Other, surrogate */
    NCC_UNICODE_GC_CO, /**< Other, private use */
    NCC_UNICODE_GC_CN, /**< Other, not assigned */
} ncc_unicode_gc_t;

// ============================================================================
// Bidi_Class
// ============================================================================

typedef enum {
    NCC_UNICODE_BIDI_L,
    NCC_UNICODE_BIDI_R,
    NCC_UNICODE_BIDI_AL,
    NCC_UNICODE_BIDI_EN,
    NCC_UNICODE_BIDI_ES,
    NCC_UNICODE_BIDI_ET,
    NCC_UNICODE_BIDI_AN,
    NCC_UNICODE_BIDI_CS,
    NCC_UNICODE_BIDI_NSM,
    NCC_UNICODE_BIDI_BN,
    NCC_UNICODE_BIDI_B,
    NCC_UNICODE_BIDI_S,
    NCC_UNICODE_BIDI_WS,
    NCC_UNICODE_BIDI_ON,
    NCC_UNICODE_BIDI_LRE,
    NCC_UNICODE_BIDI_LRO,
    NCC_UNICODE_BIDI_RLE,
    NCC_UNICODE_BIDI_RLO,
    NCC_UNICODE_BIDI_PDF,
    NCC_UNICODE_BIDI_LRI,
    NCC_UNICODE_BIDI_RLI,
    NCC_UNICODE_BIDI_FSI,
    NCC_UNICODE_BIDI_PDI,
} ncc_unicode_bidi_class_t;

// ============================================================================
// East_Asian_Width
// ============================================================================

typedef enum {
    NCC_UNICODE_EAW_N,
    NCC_UNICODE_EAW_NA,
    NCC_UNICODE_EAW_H,
    NCC_UNICODE_EAW_W,
    NCC_UNICODE_EAW_F,
    NCC_UNICODE_EAW_A,
} ncc_unicode_eaw_t;

// ============================================================================
// Script / Block
// ============================================================================

typedef uint8_t  ncc_unicode_script_t;
typedef uint16_t ncc_unicode_block_t;

// ============================================================================
// Joining_Type
// ============================================================================

typedef enum {
    NCC_UNICODE_JT_U,
    NCC_UNICODE_JT_C,
    NCC_UNICODE_JT_D,
    NCC_UNICODE_JT_L,
    NCC_UNICODE_JT_R,
    NCC_UNICODE_JT_T,
} ncc_unicode_jt_t;

// ============================================================================
// Binary properties
// ============================================================================

typedef enum {
    NCC_UNICODE_PROP_WHITE_SPACE,
    NCC_UNICODE_PROP_ALPHABETIC,
    NCC_UNICODE_PROP_NONCHARACTER_CODE_POINT,
    NCC_UNICODE_PROP_DEFAULT_IGNORABLE_CODE_POINT,
    NCC_UNICODE_PROP_DEPRECATED,
    NCC_UNICODE_PROP_LOGICAL_ORDER_EXCEPTION,
    NCC_UNICODE_PROP_VARIATION_SELECTOR,
    NCC_UNICODE_PROP_UPPERCASE,
    NCC_UNICODE_PROP_LOWERCASE,
    NCC_UNICODE_PROP_SOFT_DOTTED,
    NCC_UNICODE_PROP_CASE_IGNORABLE,
    NCC_UNICODE_PROP_CASED,
    NCC_UNICODE_PROP_CHANGES_WHEN_LOWERCASED,
    NCC_UNICODE_PROP_CHANGES_WHEN_UPPERCASED,
    NCC_UNICODE_PROP_CHANGES_WHEN_TITLECASED,
    NCC_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED,
    NCC_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED,
    NCC_UNICODE_PROP_ID_START,
    NCC_UNICODE_PROP_ID_CONTINUE,
    NCC_UNICODE_PROP_XID_START,
    NCC_UNICODE_PROP_XID_CONTINUE,
    NCC_UNICODE_PROP_PATTERN_SYNTAX,
    NCC_UNICODE_PROP_PATTERN_WHITE_SPACE,
    NCC_UNICODE_PROP_DASH,
    NCC_UNICODE_PROP_QUOTATION_MARK,
    NCC_UNICODE_PROP_TERMINAL_PUNCTUATION,
    NCC_UNICODE_PROP_SENTENCE_TERMINAL,
    NCC_UNICODE_PROP_DIACRITIC,
    NCC_UNICODE_PROP_EXTENDER,
    NCC_UNICODE_PROP_GRAPHEME_BASE,
    NCC_UNICODE_PROP_GRAPHEME_EXTEND,
    NCC_UNICODE_PROP_GRAPHEME_LINK,
    NCC_UNICODE_PROP_MATH,
    NCC_UNICODE_PROP_HEX_DIGIT,
    NCC_UNICODE_PROP_ASCII_HEX_DIGIT,
    NCC_UNICODE_PROP_IDEOGRAPHIC,
    NCC_UNICODE_PROP_UNIFIED_IDEOGRAPH,
    NCC_UNICODE_PROP_RADICAL,
    NCC_UNICODE_PROP_IDS_BINARY_OPERATOR,
    NCC_UNICODE_PROP_IDS_TRINARY_OPERATOR,
    NCC_UNICODE_PROP_JOIN_CONTROL,
    NCC_UNICODE_PROP_EMOJI,
    NCC_UNICODE_PROP_EMOJI_PRESENTATION,
    NCC_UNICODE_PROP_EMOJI_MODIFIER,
    NCC_UNICODE_PROP_EMOJI_MODIFIER_BASE,
    NCC_UNICODE_PROP_EMOJI_COMPONENT,
    NCC_UNICODE_PROP_EXTENDED_PICTOGRAPHIC,
} ncc_unicode_property_t;

// ============================================================================
// Numeric types
// ============================================================================

typedef enum {
    NCC_UNICODE_NUMERIC_NONE,
    NCC_UNICODE_NUMERIC_DECIMAL,
    NCC_UNICODE_NUMERIC_DIGIT,
    NCC_UNICODE_NUMERIC_NUMERIC,
} ncc_unicode_numeric_type_t;

typedef struct {
    ncc_unicode_numeric_type_t type;
    int32_t                     numerator;
    int32_t                     denominator;
} ncc_unicode_numeric_value_t;

// ============================================================================
// BOM detection
// ============================================================================

typedef enum {
    NCC_UNICODE_BOM_NONE,
    NCC_UNICODE_BOM_UTF8,
    NCC_UNICODE_BOM_UTF16_LE,
    NCC_UNICODE_BOM_UTF16_BE,
    NCC_UNICODE_BOM_UTF32_LE,
    NCC_UNICODE_BOM_UTF32_BE,
} ncc_unicode_bom_t;

// ============================================================================
// Stub implementations (ASCII-only)
// ============================================================================

/**
 * @brief Return the General_Category of a codepoint (ASCII-only stub).
 */
static inline ncc_unicode_gc_t
ncc_unicode_general_category(ncc_codepoint_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return NCC_UNICODE_GC_LU;
    }
    if (cp >= 'a' && cp <= 'z') {
        return NCC_UNICODE_GC_LL;
    }
    if (cp >= '0' && cp <= '9') {
        return NCC_UNICODE_GC_ND;
    }
    if (cp == ' ') {
        return NCC_UNICODE_GC_ZS;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return NCC_UNICODE_GC_CC;
    }
    // Default: treat remaining ASCII punctuation/symbols as PO.
    if (cp < 0x80) {
        return NCC_UNICODE_GC_PO;
    }
    // Non-ASCII: unassigned.
    return NCC_UNICODE_GC_CN;
}

/**
 * @brief Test whether a codepoint has a given binary property (stub).
 *
 * Only handles the most common ASCII cases; returns false otherwise.
 */
static inline bool
ncc_unicode_has_property(ncc_codepoint_t cp,
                          ncc_unicode_property_t prop)
{
    switch (prop) {
    case NCC_UNICODE_PROP_WHITE_SPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v';
    case NCC_UNICODE_PROP_ALPHABETIC:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case NCC_UNICODE_PROP_UPPERCASE:
        return cp >= 'A' && cp <= 'Z';
    case NCC_UNICODE_PROP_LOWERCASE:
        return cp >= 'a' && cp <= 'z';
    case NCC_UNICODE_PROP_HEX_DIGIT:
    case NCC_UNICODE_PROP_ASCII_HEX_DIGIT:
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'F')
            || (cp >= 'a' && cp <= 'f');
    case NCC_UNICODE_PROP_ID_START:
    case NCC_UNICODE_PROP_XID_START:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || cp == '_';
    case NCC_UNICODE_PROP_ID_CONTINUE:
    case NCC_UNICODE_PROP_XID_CONTINUE:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')
            || (cp >= '0' && cp <= '9') || cp == '_';
    default:
        return false;
    }
}

/**
 * @brief Return the display column width of a codepoint (ASCII stub).
 */
static inline int
ncc_unicode_char_width(ncc_codepoint_t cp)
{
    if (cp == 0) {
        return 0;
    }
    if (cp < 0x20 || cp == 0x7F) {
        return 0; // Control characters.
    }
    return 1;
}
