// unicode_class.c - Character class dispatch for parser engines.
//
// Uses n00b's unicode property tables for proper Unicode support
// instead of locale-dependent wctype.h functions.

#include "internal/parse/unicode_class.h"

// ============================================================================
// ASCII helpers (no table lookup needed)
// ============================================================================

static bool
is_ascii_digit(int32_t cp)
{
    return cp >= '0' && cp <= '9';
}

static bool
is_ascii_upper(int32_t cp)
{
    return cp >= 'A' && cp <= 'Z';
}

static bool
is_ascii_lower(int32_t cp)
{
    return cp >= 'a' && cp <= 'z';
}

static bool
is_ascii_alpha(int32_t cp)
{
    return is_ascii_upper(cp) || is_ascii_lower(cp);
}

static bool
is_hex_digit(int32_t cp)
{
    return is_ascii_digit(cp) || (cp >= 'a' && cp <= 'f')
        || (cp >= 'A' && cp <= 'F');
}

static bool
is_nonzero_digit(int32_t cp)
{
    return cp >= '1' && cp <= '9';
}

// ============================================================================
// Unicode-aware helpers (via n00b property tables)
// ============================================================================

static bool
is_whitespace(int32_t cp)
{
    return ncc_unicode_has_property((ncc_codepoint_t)cp,
                                     NCC_UNICODE_PROP_WHITE_SPACE);
}

static bool
is_printable(int32_t cp)
{
    if (cp < 0) {
        return false;
    }

    ncc_unicode_gc_t gc = ncc_unicode_general_category((ncc_codepoint_t)cp);

    // Control characters (Cc) are not printable.
    if (gc == NCC_UNICODE_GC_CC) {
        return false;
    }

    // Unassigned (Cn) and surrogates (Cs) are not printable.
    if (gc == NCC_UNICODE_GC_CN || gc == NCC_UNICODE_GC_CS) {
        return false;
    }

    return true;
}

static bool
is_non_ws_printable(int32_t cp)
{
    return is_printable(cp) && !is_whitespace(cp);
}

static bool
is_non_nl_ws(int32_t cp)
{
    return is_whitespace(cp) && cp != '\n' && cp != '\r';
}

static bool
is_non_nl_printable(int32_t cp)
{
    return is_printable(cp) && cp != '\n' && cp != '\r';
}

static bool
is_json_string_char(int32_t cp)
{
    // Valid unescaped JSON string character:
    // any Unicode character except " and \ and control chars (0x00-0x1F).
    if (cp < 0x20) {
        return false;
    }

    if (cp == '"' || cp == '\\') {
        return false;
    }

    return true;
}

// ============================================================================
// Public dispatch
// ============================================================================

bool
ncc_codepoint_matches_class(int32_t cp, ncc_char_class_t cc)
{
    switch (cc) {
    case NCC_CC_ID_START:
        return ncc_unicode_is_id_start((ncc_codepoint_t)cp);
    case NCC_CC_ID_CONTINUE:
        return ncc_unicode_is_id_continue((ncc_codepoint_t)cp);
    case NCC_CC_ASCII_DIGIT:
        return is_ascii_digit(cp);
    case NCC_CC_UNICODE_DIGIT:
        return ncc_unicode_general_category((ncc_codepoint_t)cp) == NCC_UNICODE_GC_ND;
    case NCC_CC_ASCII_UPPER:
        return is_ascii_upper(cp);
    case NCC_CC_ASCII_LOWER:
        return is_ascii_lower(cp);
    case NCC_CC_ASCII_ALPHA:
        return is_ascii_alpha(cp);
    case NCC_CC_WHITESPACE:
        return is_whitespace(cp);
    case NCC_CC_HEX_DIGIT:
        return is_hex_digit(cp);
    case NCC_CC_NONZERO_DIGIT:
        return is_nonzero_digit(cp);
    case NCC_CC_PRINTABLE:
        return is_printable(cp);
    case NCC_CC_NON_WS_PRINTABLE:
        return is_non_ws_printable(cp);
    case NCC_CC_NON_NL_WS:
        return is_non_nl_ws(cp);
    case NCC_CC_NON_NL_PRINTABLE:
        return is_non_nl_printable(cp);
    case NCC_CC_JSON_STRING_CHAR:
        return is_json_string_char(cp);
    case NCC_CC_REGEX_BODY_CHAR:
        return is_printable(cp) && cp != '/';
    }

    return false;
}
