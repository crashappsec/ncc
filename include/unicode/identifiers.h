#pragma once
/**
 * @file identifiers.h
 * @brief ASCII-only identifier character classification (standalone stub).
 *
 * The full n00b runtime performs full Unicode ID_Start / ID_Continue
 * classification. This stub handles ASCII only, which is sufficient
 * for the parser's grammar-level identifier needs.
 */

#include "ncc.h"

static inline bool
ncc_unicode_is_id_start(ncc_codepoint_t cp)
{
    return (cp >= 'A' && cp <= 'Z')
        || (cp >= 'a' && cp <= 'z')
        || cp == '_';
}

static inline bool
ncc_unicode_is_id_continue(ncc_codepoint_t cp)
{
    return ncc_unicode_is_id_start(cp)
        || (cp >= '0' && cp <= '9');
}

static inline bool
ncc_unicode_is_xid_start(ncc_codepoint_t cp)
{
    return ncc_unicode_is_id_start(cp);
}

static inline bool
ncc_unicode_is_xid_continue(ncc_codepoint_t cp)
{
    return ncc_unicode_is_id_continue(cp);
}

static inline bool
ncc_unicode_is_pattern_syntax(ncc_codepoint_t cp)
{
    // ASCII pattern syntax characters.
    return (cp >= '!' && cp <= '/')
        || (cp >= ':' && cp <= '@')
        || (cp >= '[' && cp <= '^')
        || cp == '`'
        || (cp >= '{' && cp <= '~');
}

static inline bool
ncc_unicode_is_pattern_white_space(ncc_codepoint_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
        || cp == '\f' || cp == '\v';
}

static inline bool
ncc_unicode_is_identifier_allowed(ncc_codepoint_t cp)
{
    return ncc_unicode_is_id_start(cp) || ncc_unicode_is_id_continue(cp);
}
