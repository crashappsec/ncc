// bnf.c - BNF grammar-from-text parser
//
// Ported from n00b/src/slay/bnf.c with ncc→C23 translation:
// - *r"..." → NCC_STRING_STATIC(...)
// - ncc_unicode_str_eq() → ncc_string_eq()
// - _kargs removed from public API
// - Always uses PWZ parser

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/scanner.h"
#include "scanner/scan_builtins.h"
#include "scanner/token_stream.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Simple UTF-8 decoder for grammar literals
// ============================================================================

static int32_t
utf8_decode(const char **pp)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int32_t              cp;

    if (*p < 0x80) {
        cp = *p++;
    }
    else if (*p < 0xE0) {
        cp  = (*p++ & 0x1F) << 6;
        cp |= *p++ & 0x3F;
    }
    else if (*p < 0xF0) {
        cp  = (*p++ & 0x0F) << 12;
        cp |= (*p++ & 0x3F) << 6;
        cp |= *p++ & 0x3F;
    }
    else {
        cp  = (*p++ & 0x07) << 18;
        cp |= (*p++ & 0x3F) << 12;
        cp |= (*p++ & 0x3F) << 6;
        cp |= *p++ & 0x3F;
    }

    *pp = (const char *)p;
    return cp;
}

// ============================================================================
// Preprocessing
// ============================================================================

ncc_string_t
ncc_bnf_strip_comments(ncc_string_t input)
{
    if (!input.data) {
        return ncc_string_empty();
    }

    size_t      len    = input.u8_bytes;
    const char *src    = input.data;
    char       *result = ncc_alloc_array(char, len + 1);
    size_t      ri     = 0;
    bool        in_sq  = false;
    bool        in_dq  = false;

    for (size_t i = 0; i < len; i++) {
        char c = src[i];

        if (c == '\'' && !in_dq) {
            in_sq = !in_sq;
        }
        else if (c == '"' && !in_sq) {
            in_dq = !in_dq;
        }

        if (c == '#' && !in_sq && !in_dq) {
            while (i < len && src[i] != '\n') {
                i++;
            }

            if (i < len) {
                result[ri++] = '\n';
            }

            continue;
        }

        result[ri++] = c;
    }

    result[ri] = '\0';

    return ncc_string_from_raw(result, (int64_t)ri);
}

ncc_string_t
ncc_bnf_trim_lines(ncc_string_t input)
{
    if (!input.data) {
        return ncc_string_empty();
    }

    size_t      len    = input.u8_bytes;
    const char *src    = input.data;
    const char *end    = src + len;
    char       *result = ncc_alloc_array(char, len + 1);
    size_t      ri     = 0;
    const char *p      = src;

    while (p < end) {
        while (p < end && *p != '\n' && (*p == ' ' || *p == '\t')) {
            p++;
        }

        const char *line_start = p;

        while (p < end && *p != '\n') {
            p++;
        }

        const char *line_end = p;

        while (line_end > line_start
               && (line_end[-1] == ' ' || line_end[-1] == '\t')) {
            line_end--;
        }

        size_t line_len = (size_t)(line_end - line_start);
        memcpy(result + ri, line_start, line_len);
        ri += line_len;

        if (p < end && *p == '\n') {
            result[ri++] = '\n';
            p++;
        }
    }

    result[ri] = '\0';

    return ncc_string_from_raw(result, (int64_t)ri);
}

// ============================================================================
// Join continuation lines: remove newlines before lines starting with '|'
// ============================================================================

static ncc_string_t
bnf_join_continuations(ncc_string_t input)
{
    if (!input.data) {
        return ncc_string_empty();
    }

    size_t      len    = input.u8_bytes;
    const char *src    = input.data;
    char       *result = ncc_alloc_array(char, len + 1);
    size_t      ri     = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            size_t j = i + 1;

            while (j < len && (src[j] == ' ' || src[j] == '\t')) {
                j++;
            }

            if (j < len && src[j] == '|') {
                result[ri++] = ' ';
                i = j - 1;
                continue;
            }
        }

        result[ri++] = src[i];
    }

    result[ri] = '\0';

    return ncc_string_from_raw(result, (int64_t)ri);
}

// ============================================================================
// BNF Tokenizer
// ============================================================================

// Token IDs for the BNF meta-grammar.
enum {
    BNF_TOK_LANGLE     = NCC_TOK_START_ID + 1,
    BNF_TOK_RANGLE     = NCC_TOK_START_ID + 2,
    BNF_TOK_ASSIGN     = NCC_TOK_START_ID + 3,
    BNF_TOK_PIPE       = NCC_TOK_START_ID + 4,
    BNF_TOK_NEWLINE    = NCC_TOK_START_ID + 5,
    BNF_TOK_NAME       = NCC_TOK_START_ID + 6,
    BNF_TOK_LITERAL    = NCC_TOK_START_ID + 7,
    BNF_TOK_CLASS      = NCC_TOK_START_ID + 8,
    BNF_TOK_TOKEN_TYPE = NCC_TOK_START_ID + 9,
    BNF_TOK_TOKEN_LIT  = NCC_TOK_START_ID + 10,
    BNF_TOK_EMPTY_LIT  = NCC_TOK_START_ID + 11,
    BNF_TOK_QUESTION   = NCC_TOK_START_ID + 12,
    BNF_TOK_STAR       = NCC_TOK_START_ID + 13,
    BNF_TOK_PLUS_OP    = NCC_TOK_START_ID + 14,
    BNF_TOK_LPAREN     = NCC_TOK_START_ID + 15,
    BNF_TOK_RPAREN     = NCC_TOK_START_ID + 16,
    BNF_TOK_AT         = NCC_TOK_START_ID + 17,
    BNF_TOK_DOLLAR     = NCC_TOK_START_ID + 18,
    BNF_TOK_COMMA      = NCC_TOK_START_ID + 19,
};

// ============================================================================
// BNF scanner callback
// ============================================================================

static bool
is_bnf_hws(ncc_codepoint_t cp, void *ctx)
{
    (void)ctx;
    return cp == ' ' || cp == '\t';
}

static bool
bnf_scan(ncc_scanner_t *s)
{
    bool *in_angle = (bool *)s->user_state;

    ncc_scan_skip_while(s, is_bnf_hws);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_codepoint_t ch = ncc_scan_peek(s, 0);

    // Newline.
    if (ch == '\r' || ch == '\n') {
        ncc_scan_mark(s);

        if (ch == '\r' && ncc_scan_peek_byte(s, 1) == '\n') {
            ncc_scan_advance(s);
        }

        ncc_scan_advance(s);
        ncc_scan_emit_marked(s, BNF_TOK_NEWLINE);
        return true;
    }

    // ::=
    if (ch == ':') {
        ncc_scan_mark(s);

        if (ncc_scan_match_str(s, "::=")) {
            ncc_scan_emit_marked(s, BNF_TOK_ASSIGN);
            return true;
        }

        ncc_scan_advance(s);
        return true;
    }

    // < and >
    if (ch == '<') {
        ncc_scan_mark(s);
        ncc_scan_advance(s);
        ncc_scan_emit_marked(s, BNF_TOK_LANGLE);
        *in_angle = true;
        return true;
    }

    if (ch == '>') {
        ncc_scan_mark(s);
        ncc_scan_advance(s);
        ncc_scan_emit_marked(s, BNF_TOK_RANGLE);
        *in_angle = false;
        return true;
    }

    // |
    if (ch == '|') {
        ncc_scan_mark(s);
        ncc_scan_advance(s);
        ncc_scan_emit_marked(s, BNF_TOK_PIPE);
        return true;
    }

    // Token terminal: %NAME or %"..." or %'...'
    if (ch == '%') {
        ncc_scan_advance(s);

        ncc_codepoint_t next = ncc_scan_peek(s, 0);

        if (next == '"' || next == '\'') {
            ncc_codepoint_t quote = next;
            ncc_scan_advance(s);
            ncc_scan_mark(s);

            while (!ncc_scan_at_eof(s)) {
                ncc_codepoint_t c = ncc_scan_peek(s, 0);

                if (c == quote || c == '\n') {
                    break;
                }

                ncc_scan_advance(s);
            }

            ncc_string_t val = ncc_scan_extract(s);

            if (!ncc_scan_at_eof(s) && ncc_scan_peek(s, 0) == quote) {
                ncc_scan_advance(s);
            }

            ncc_scan_emit(s, BNF_TOK_TOKEN_LIT,
                           ncc_option_set(ncc_string_t, val));
            return true;
        }

        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')
            || next == '_') {
            ncc_scan_mark(s);

            while (!ncc_scan_at_eof(s)) {
                ncc_codepoint_t c = ncc_scan_peek(s, 0);

                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || (c >= '0' && c <= '9') || c == '_')) {
                    break;
                }

                ncc_scan_advance(s);
            }

            ncc_string_t val = ncc_scan_extract(s);
            ncc_scan_emit(s, BNF_TOK_TOKEN_TYPE,
                           ncc_option_set(ncc_string_t, val));
            return true;
        }

        return true;
    }

    // Quoted literal: "..." or '...'
    if (ch == '"' || ch == '\'') {
        ncc_codepoint_t quote = ch;
        ncc_scan_advance(s);
        ncc_scan_mark(s);

        while (!ncc_scan_at_eof(s)) {
            ncc_codepoint_t c = ncc_scan_peek(s, 0);

            if (c == quote || c == '\n') {
                break;
            }

            ncc_scan_advance(s);
        }

        ncc_string_t val = ncc_scan_extract(s);

        if (!ncc_scan_at_eof(s) && ncc_scan_peek(s, 0) == quote) {
            ncc_scan_advance(s);
        }

        int32_t tid = (val.u8_bytes == 0) ? BNF_TOK_EMPTY_LIT : BNF_TOK_LITERAL;
        ncc_scan_emit(s, tid, ncc_option_set(ncc_string_t, val));
        return true;
    }

    // __CLASS (reserved terminal)
    if (ch == '_' && ncc_scan_peek_byte(s, 1) == '_') {
        uint8_t third = ncc_scan_peek_byte(s, 2);

        if ((third >= 'a' && third <= 'z') || (third >= 'A' && third <= 'Z')) {
            ncc_scan_mark(s);
            ncc_scan_advance(s);
            ncc_scan_advance(s);

            while (!ncc_scan_at_eof(s)) {
                ncc_codepoint_t c = ncc_scan_peek(s, 0);

                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                      || c == '_')) {
                    break;
                }

                ncc_scan_advance(s);
            }

            ncc_string_t val = ncc_scan_extract(s);
            ncc_scan_emit(s, BNF_TOK_CLASS,
                           ncc_option_set(ncc_string_t, val));
            return true;
        }
    }

    // Rule name
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
        ncc_scan_mark(s);

        while (!ncc_scan_at_eof(s)) {
            ncc_codepoint_t c = ncc_scan_peek(s, 0);

            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '-' || c == '_'
                  || (*in_angle && c == ','))) {
                break;
            }

            ncc_scan_advance(s);
        }

        ncc_string_t val = ncc_scan_extract(s);
        ncc_scan_emit(s, BNF_TOK_NAME,
                       ncc_option_set(ncc_string_t, val));
        return true;
    }

    // Single-character tokens.
    ncc_scan_mark(s);
    ncc_scan_advance(s);

    switch (ch) {
    case '?': ncc_scan_emit_marked(s, BNF_TOK_QUESTION); return true;
    case '*': ncc_scan_emit_marked(s, BNF_TOK_STAR);     return true;
    case '+': ncc_scan_emit_marked(s, BNF_TOK_PLUS_OP);  return true;
    case '(': ncc_scan_emit_marked(s, BNF_TOK_LPAREN);   return true;
    case ')': ncc_scan_emit_marked(s, BNF_TOK_RPAREN);   return true;
    case '@': ncc_scan_emit_marked(s, BNF_TOK_AT);       return true;
    case ',': ncc_scan_emit_marked(s, BNF_TOK_COMMA);    return true;
    default:  break;
    }

    // $N (child reference)
    if (ch == '$') {
        ncc_codepoint_t d = ncc_scan_peek(s, 0);

        if (d >= '0' && d <= '9') {
            ncc_scan_mark(s);

            while (!ncc_scan_at_eof(s)) {
                ncc_codepoint_t c = ncc_scan_peek(s, 0);

                if (c < '0' || c > '9') {
                    break;
                }

                ncc_scan_advance(s);
            }

            ncc_string_t val = ncc_scan_extract(s);
            ncc_scan_emit(s, BNF_TOK_DOLLAR,
                           ncc_option_set(ncc_string_t, val));
            return true;
        }
    }

    return true;
}

// ============================================================================
// Helper: extract ncc_string_t from token value
// ============================================================================

static ncc_string_t
tok_str(ncc_token_info_t *tok)
{
    if (!tok || !ncc_option_is_set(tok->value)) {
        return (ncc_string_t){0};
    }

    return ncc_option_get(tok->value);
}

static ncc_string_t
tok_str_or(ncc_token_info_t *tok, const char *fallback)
{
    ncc_string_t s = tok_str(tok);

    if (s.data) {
        return s;
    }

    return ncc_string_from_cstr(fallback);
}

static inline bool
str_eq_lit(ncc_string_t s, const char *lit)
{
    return ncc_string_eq(s, ncc_string_from_cstr(lit));
}

// ============================================================================
// Walk action result types
// ============================================================================

typedef struct {
    int   tag;
    void *data;
} bnf_result_t;

enum {
    BNF_STRING = 1,
    BNF_LIST   = 2,
    BNF_PAIR   = 3,
    BNF_DICT   = 4,
    BNF_GROUP  = 5,
    BNF_ANNOT  = 6,
    BNF_NAME   = 7,
};

typedef void *bnf_ptr_t;
ncc_list_decl(bnf_ptr_t);
typedef ncc_list_t(bnf_ptr_t) bnf_list_t;

typedef struct {
    bnf_list_t *alternatives;
    char        quantifier;
} bnf_group_info_t;

static inline bnf_list_t *
slist_new(void)
{
    bnf_list_t *l = ncc_alloc(bnf_list_t);
    *l = ncc_list_new(bnf_ptr_t);
    return l;
}

static inline void
slist_push(bnf_list_t *l, void *item)
{
    ncc_list_push(*l, (bnf_ptr_t)item);
}

static inline void *
slist_get(bnf_list_t *l, size_t i)
{
    return (l && i < l->len) ? l->data[i] : nullptr;
}

static inline void
slist_prepend(bnf_list_t *l, void *item)
{
    ncc_list_push_front(*l, (bnf_ptr_t)item);
}

static inline void
slist_free(bnf_list_t *l)
{
    if (l) {
        ncc_list_free(*l);
        ncc_free(l);
    }
}

static inline bnf_result_t *
bnf_result(int tag, void *data)
{
    bnf_result_t *r = ncc_alloc(bnf_result_t);
    r->tag  = tag;
    r->data = data;
    return r;
}

// ============================================================================
// Walk actions for token-based BNF grammar
// ============================================================================

static void *
bnf_walk_atom(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        char *r = ncc_alloc_array(char, 3);
        r[0] = 'L'; r[1] = ':'; r[2] = '\0';
        return bnf_result(BNF_STRING, r);
    }

    char *result;

    if (pn->rule_index == 0) {
        ncc_token_info_t *tok = (ncc_token_info_t *)kids[0];
        ncc_string_t      val = tok_str_or(tok, "");
        size_t             len = val.u8_bytes;
        result                 = ncc_alloc_array(char, len + 3);
        result[0]              = 'L';
        result[1]              = ':';
        memcpy(result + 2, val.data, len + 1);
    }
    else if (pn->rule_index == 1) {
        ncc_token_info_t *tok = (ncc_token_info_t *)kids[1];
        ncc_string_t      val = tok_str_or(tok, "");
        size_t             len = val.u8_bytes;
        result                 = ncc_alloc_array(char, len + 3);
        result[0]              = 'N';
        result[1]              = ':';
        memcpy(result + 2, val.data, len + 1);
    }
    else if (pn->rule_index == 2) {
        ncc_token_info_t *tok = (ncc_token_info_t *)kids[0];
        ncc_string_t      val = tok_str_or(tok, "");
        size_t             len = val.u8_bytes;
        result                 = ncc_alloc_array(char, len + 3);
        result[0]              = 'C';
        result[1]              = ':';
        memcpy(result + 2, val.data, len + 1);
    }
    else if (pn->rule_index == 3) {
        ncc_token_info_t *tok = (ncc_token_info_t *)kids[0];
        ncc_string_t      val = tok_str_or(tok, "");
        size_t             len = val.u8_bytes;
        result                 = ncc_alloc_array(char, len + 3);
        result[0]              = 'T';
        result[1]              = ':';
        memcpy(result + 2, val.data, len + 1);
    }
    else if (pn->rule_index == 4) {
        ncc_token_info_t *tok = (ncc_token_info_t *)kids[0];
        ncc_string_t      val = tok_str_or(tok, "");
        size_t             len = val.u8_bytes;
        result                 = ncc_alloc_array(char, len + 3);
        result[0]              = 'K';
        result[1]              = ':';
        memcpy(result + 2, val.data, len + 1);
    }
    else {
        result = ncc_alloc_array(char, 3);
        result[0] = 'E'; result[1] = ':'; result[2] = '\0';
    }

    ncc_free(kids);
    return bnf_result(BNF_STRING, result);
}

static void *
bnf_walk_item(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        char *s = ncc_alloc_array(char, 3);
        s[0] = 'E'; s[1] = ':'; s[2] = '\0';
        return bnf_result(BNF_STRING, s);
    }

    bnf_result_t *result;

    switch (pn->rule_index) {
    case 0:
        result = (bnf_result_t *)kids[0];
        ncc_free(kids);
        return result;

    case 1:
    case 2:
    case 3: {
        char quantifier = (pn->rule_index == 1) ? '?'
                        : (pn->rule_index == 2) ? '*'
                                                : '+';

        bnf_group_info_t *gi = ncc_alloc(bnf_group_info_t);
        gi->quantifier       = quantifier;
        gi->alternatives     = slist_new();

        bnf_list_t *terms = slist_new();
        slist_push(terms, kids[0]);
        slist_push(gi->alternatives, bnf_result(BNF_LIST, terms));

        ncc_free(kids);
        return bnf_result(BNF_GROUP, gi);
    }

    case 4: {
        bnf_result_t *expr_r = (bnf_result_t *)kids[1];

        bnf_group_info_t *gi = ncc_alloc(bnf_group_info_t);
        gi->quantifier       = 0;
        gi->alternatives     = (bnf_list_t *)expr_r->data;

        ncc_free(expr_r);
        ncc_free(kids);
        return bnf_result(BNF_GROUP, gi);
    }

    case 5:
    case 6:
    case 7: {
        char quantifier = (pn->rule_index == 5) ? '?'
                        : (pn->rule_index == 6) ? '*'
                                                : '+';

        bnf_result_t *expr_r = (bnf_result_t *)kids[1];

        bnf_group_info_t *gi = ncc_alloc(bnf_group_info_t);
        gi->quantifier       = quantifier;
        gi->alternatives     = (bnf_list_t *)expr_r->data;

        ncc_free(expr_r);
        ncc_free(kids);
        return bnf_result(BNF_GROUP, gi);
    }

    default:
        ncc_free(kids);
        {
            char *es = ncc_alloc_array(char, 3);
            es[0] = 'E'; es[1] = ':'; es[2] = '\0';
            return bnf_result(BNF_STRING, es);
        }
    }
}

static void *
bnf_walk_list(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return bnf_result(BNF_LIST, slist_new());
    }

    bnf_list_t *result;

    if (pn->rule_index == 0) {
        result = slist_new();
        slist_push(result, kids[0]);
    }
    else {
        bnf_result_t *list_r = (bnf_result_t *)kids[1];
        result               = list_r ? (bnf_list_t *)list_r->data : slist_new();
        slist_prepend(result, kids[0]);

        if (list_r) {
            ncc_free(list_r);
        }
    }

    ncc_free(kids);
    return bnf_result(BNF_LIST, result);
}

static void *
bnf_walk_expression(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return bnf_result(BNF_LIST, slist_new());
    }

    bnf_list_t *result;

    if (pn->rule_index == 0) {
        result = slist_new();
        slist_push(result, kids[0]);
    }
    else {
        bnf_result_t *expr_r = (bnf_result_t *)kids[2];
        result               = expr_r ? (bnf_list_t *)expr_r->data : slist_new();
        slist_prepend(result, kids[0]);

        if (expr_r) {
            ncc_free(expr_r);
        }
    }

    ncc_free(kids);
    return bnf_result(BNF_LIST, result);
}

// ============================================================================
// Annotation walk data (annotations parsed and discarded)
// ============================================================================

// Penalty info extracted from @penalty annotations (the only
// annotation that has a runtime effect on the grammar).
typedef struct {
    int32_t penalty_cost;
} bnf_annot_info_t;

static void *
bnf_walk_annot_arg(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)pn;
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return nullptr;
    }

    void *tok = kids[0];
    ncc_free(kids);
    return tok;
}

static void *
bnf_walk_arg_list(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return bnf_result(BNF_LIST, slist_new());
    }

    bnf_list_t *result;

    if (pn->rule_index == 0) {
        result = slist_new();
        slist_push(result, kids[0]);
    }
    else {
        bnf_result_t *list_r = (bnf_result_t *)kids[2];
        result = list_r ? (bnf_list_t *)list_r->data : slist_new();
        slist_prepend(result, kids[0]);

        if (list_r) {
            ncc_free(list_r);
        }
    }

    ncc_free(kids);
    return bnf_result(BNF_LIST, result);
}

// Parse an @annotation — only @penalty has a runtime effect; all
// others are parsed and discarded.
static void *
bnf_walk_annotation(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return nullptr;
    }

    ncc_token_info_t *name_tok = (ncc_token_info_t *)kids[1];
    bnf_result_t     *args_r   = nullptr;
    bnf_list_t       *args     = nullptr;

    if (pn->rule_index == 0) {
        args_r = (bnf_result_t *)kids[3];
        args   = args_r ? (bnf_list_t *)args_r->data : nullptr;
    }

    bnf_annot_info_t *info = ncc_alloc(bnf_annot_info_t);
    info->penalty_cost = 0;

    ncc_string_t annot_str = tok_str(name_tok);

    if (str_eq_lit(annot_str, "penalty")) {
        if (args && args->len >= 1) {
            ncc_token_info_t *cost_tok = (ncc_token_info_t *)slist_get(args, 0);
            ncc_string_t      cost_val = tok_str(cost_tok);
            if (cost_val.data)
                info->penalty_cost = (int32_t)strtol(cost_val.data, nullptr, 10);
        }
        if (info->penalty_cost <= 0)
            info->penalty_cost = 1;
    }

    if (args_r) {
        slist_free(args);
        ncc_free(args_r);
    }

    ncc_free(kids);
    return bnf_result(BNF_ANNOT, info);
}

static void *
bnf_walk_annotations(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return bnf_result(BNF_LIST, slist_new());
    }

    bnf_list_t *result;

    if (pn->rule_index == 0) {
        result = slist_new();
    }
    else {
        bnf_result_t *rest_r = (bnf_result_t *)kids[1];
        result = rest_r ? (bnf_list_t *)rest_r->data : slist_new();
        slist_prepend(result, kids[0]);

        if (rest_r) {
            ncc_free(rest_r);
        }
    }

    ncc_free(kids);
    return bnf_result(BNF_LIST, result);
}

static void *
bnf_walk_rule(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)pn;
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return nullptr;
    }

    ncc_token_info_t *name_tok   = (ncc_token_info_t *)kids[1];
    bnf_result_t      *annots_r   = (bnf_result_t *)kids[3];
    bnf_result_t      *expr_r     = (bnf_result_t *)kids[5];

    ncc_string_t  name_s    = tok_str_or(name_tok, "?");
    ncc_string_t *heap_name = ncc_alloc(ncc_string_t);
    *heap_name = name_s;

    bnf_list_t *triple = slist_new();
    slist_push(triple, bnf_result(BNF_NAME, heap_name));
    slist_push(triple, expr_r);
    slist_push(triple, annots_r);

    ncc_free(kids);
    return bnf_result(BNF_PAIR, triple);
}

static void *
bnf_walk_syntax(ncc_nt_node_t *pn, void *children, void *thunk)
{
    (void)thunk;
    void **kids = (void **)children;

    if (!kids) {
        return bnf_result(BNF_DICT, slist_new());
    }

    bnf_list_t *result;

    if (pn->rule_index == 0) {
        result = slist_new();
        if (kids[0])
            slist_push(result, kids[0]);
    }
    else if (pn->rule_index == 1) {
        bnf_result_t *syntax_r = (bnf_result_t *)kids[1];
        result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
        if (kids[0])
            slist_prepend(result, kids[0]);
        if (syntax_r)
            ncc_free(syntax_r);
    }
    else if (pn->rule_index == 2) {
        bnf_result_t *syntax_r = (bnf_result_t *)kids[1];
        result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
        if (syntax_r)
            ncc_free(syntax_r);
    }
    else if (pn->rule_index == 3) {
        result = slist_new();
    }
    else if (pn->rule_index == 4) {
        bnf_result_t *syntax_r = (bnf_result_t *)kids[2];
        result = syntax_r ? (bnf_list_t *)syntax_r->data : slist_new();
        if (syntax_r)
            ncc_free(syntax_r);

        if (kids[0]) {
            bnf_list_t *annot_list = slist_new();
            slist_push(annot_list, kids[0]);
            bnf_result_t *annots_r = bnf_result(BNF_LIST, annot_list);

            bnf_list_t *triple = slist_new();
            {
                ncc_string_t *empty = ncc_alloc(ncc_string_t);
                *empty = ncc_string_empty();
                slist_push(triple, bnf_result(BNF_NAME, empty));
            }
            slist_push(triple, nullptr);
            slist_push(triple, annots_r);
            slist_prepend(result, bnf_result(BNF_PAIR, triple));
        }
    }
    else {
        result = slist_new();

        if (kids[0]) {
            bnf_list_t *annot_list = slist_new();
            slist_push(annot_list, kids[0]);
            bnf_result_t *annots_r = bnf_result(BNF_LIST, annot_list);

            bnf_list_t *triple = slist_new();
            {
                ncc_string_t *empty = ncc_alloc(ncc_string_t);
                *empty = ncc_string_empty();
                slist_push(triple, bnf_result(BNF_NAME, empty));
            }
            slist_push(triple, nullptr);
            slist_push(triple, annots_r);
            slist_push(result, bnf_result(BNF_PAIR, triple));
        }
    }

    ncc_free(kids);
    return bnf_result(BNF_DICT, result);
}

// ============================================================================
// Build the BNF meta-grammar (token-level)
// ============================================================================

static ncc_grammar_t *
build_bnf_grammar(void)
{
    ncc_grammar_t *g = ncc_grammar_new();
    ncc_grammar_set_error_recovery(g, false);

    int64_t LANGLE     = BNF_TOK_LANGLE;
    int64_t RANGLE     = BNF_TOK_RANGLE;
    int64_t ASSIGN     = BNF_TOK_ASSIGN;
    int64_t PIPE       = BNF_TOK_PIPE;
    int64_t NEWLINE    = BNF_TOK_NEWLINE;
    int64_t NAME       = BNF_TOK_NAME;
    int64_t LITERAL    = BNF_TOK_LITERAL;
    int64_t CLASS      = BNF_TOK_CLASS;
    int64_t TOKEN_TYPE = BNF_TOK_TOKEN_TYPE;
    int64_t TOKEN_LIT  = BNF_TOK_TOKEN_LIT;
    int64_t EMPTY_LIT  = BNF_TOK_EMPTY_LIT;
    int64_t QUESTION   = BNF_TOK_QUESTION;
    int64_t STAR       = BNF_TOK_STAR;
    int64_t PLUS_OP    = BNF_TOK_PLUS_OP;
    int64_t LPAREN     = BNF_TOK_LPAREN;
    int64_t RPAREN     = BNF_TOK_RPAREN;
    int64_t AT         = BNF_TOK_AT;
    int64_t DOLLAR     = BNF_TOK_DOLLAR;
    int64_t COMMA      = BNF_TOK_COMMA;

    // Create all non-terminals (NCC_STRING_STATIC replaces ncc *r"...").
    ncc_string_t s_syntax      = NCC_STRING_STATIC("syntax");
    ncc_string_t s_rule        = NCC_STRING_STATIC("rule");
    ncc_string_t s_expression  = NCC_STRING_STATIC("expression");
    ncc_string_t s_list        = NCC_STRING_STATIC("list");
    ncc_string_t s_item        = NCC_STRING_STATIC("item");
    ncc_string_t s_atom        = NCC_STRING_STATIC("atom");
    ncc_string_t s_annotations = NCC_STRING_STATIC("annotations");
    ncc_string_t s_annotation  = NCC_STRING_STATIC("annotation");
    ncc_string_t s_arg_list    = NCC_STRING_STATIC("arg-list");
    ncc_string_t s_annot_arg   = NCC_STRING_STATIC("annot-arg");

    ncc_nonterm(g, s_syntax);
    ncc_nonterm(g, s_rule);
    ncc_nonterm(g, s_expression);
    ncc_nonterm(g, s_list);
    ncc_nonterm(g, s_item);
    ncc_nonterm(g, s_atom);
    ncc_nonterm(g, s_annotations);
    ncc_nonterm(g, s_annotation);
    ncc_nonterm(g, s_arg_list);
    ncc_nonterm(g, s_annot_arg);

    ncc_nonterm_t *syntax      = ncc_nonterm(g, s_syntax);
    ncc_nonterm_t *rule        = ncc_nonterm(g, s_rule);
    ncc_nonterm_t *expression  = ncc_nonterm(g, s_expression);
    ncc_nonterm_t *list        = ncc_nonterm(g, s_list);
    ncc_nonterm_t *item        = ncc_nonterm(g, s_item);
    ncc_nonterm_t *atom        = ncc_nonterm(g, s_atom);
    ncc_nonterm_t *annotations = ncc_nonterm(g, s_annotations);
    ncc_nonterm_t *annotation  = ncc_nonterm(g, s_annotation);
    ncc_nonterm_t *arg_list    = ncc_nonterm(g, s_arg_list);
    ncc_nonterm_t *annot_arg   = ncc_nonterm(g, s_annot_arg);

    ncc_grammar_set_start(g, syntax);

    ncc_add_rule(g, syntax, NCC_NT(rule));
    ncc_add_rule(g, syntax, NCC_NT(rule), NCC_NT(syntax));
    ncc_add_rule(g, syntax, NCC_TERMINAL(NEWLINE), NCC_NT(syntax));
    ncc_add_rule(g, syntax, NCC_TERMINAL(NEWLINE));
    ncc_add_rule(g, syntax, NCC_NT(annotation), NCC_TERMINAL(NEWLINE),
                  NCC_NT(syntax));
    ncc_add_rule(g, syntax, NCC_NT(annotation), NCC_TERMINAL(NEWLINE));

    ncc_add_rule(g, rule, NCC_TERMINAL(LANGLE), NCC_TERMINAL(NAME),
                  NCC_TERMINAL(RANGLE), NCC_NT(annotations),
                  NCC_TERMINAL(ASSIGN),
                  NCC_NT(expression), NCC_TERMINAL(NEWLINE));

    ncc_add_rule(g, expression, NCC_NT(list));
    ncc_add_rule(g, expression, NCC_NT(list), NCC_TERMINAL(PIPE),
                  NCC_NT(expression));

    ncc_add_rule(g, list, NCC_NT(item));
    ncc_add_rule(g, list, NCC_NT(item), NCC_NT(list));

    ncc_add_rule(g, item, NCC_NT(atom));
    ncc_add_rule(g, item, NCC_NT(atom), NCC_TERMINAL(QUESTION));
    ncc_add_rule(g, item, NCC_NT(atom), NCC_TERMINAL(STAR));
    ncc_add_rule(g, item, NCC_NT(atom), NCC_TERMINAL(PLUS_OP));
    ncc_add_rule(g, item, NCC_TERMINAL(LPAREN), NCC_NT(expression),
                  NCC_TERMINAL(RPAREN));
    ncc_add_rule(g, item, NCC_TERMINAL(LPAREN), NCC_NT(expression),
                  NCC_TERMINAL(RPAREN), NCC_TERMINAL(QUESTION));
    ncc_add_rule(g, item, NCC_TERMINAL(LPAREN), NCC_NT(expression),
                  NCC_TERMINAL(RPAREN), NCC_TERMINAL(STAR));
    ncc_add_rule(g, item, NCC_TERMINAL(LPAREN), NCC_NT(expression),
                  NCC_TERMINAL(RPAREN), NCC_TERMINAL(PLUS_OP));

    ncc_add_rule(g, atom, NCC_TERMINAL(LITERAL));
    ncc_add_rule(g, atom, NCC_TERMINAL(LANGLE), NCC_TERMINAL(NAME),
                  NCC_TERMINAL(RANGLE));
    ncc_add_rule(g, atom, NCC_TERMINAL(CLASS));
    ncc_add_rule(g, atom, NCC_TERMINAL(TOKEN_TYPE));
    ncc_add_rule(g, atom, NCC_TERMINAL(TOKEN_LIT));
    ncc_add_rule(g, atom, NCC_TERMINAL(EMPTY_LIT));

    ncc_add_rule(g, annotations, NCC_EPSILON());
    ncc_add_rule(g, annotations, NCC_NT(annotation), NCC_NT(annotations));

    ncc_add_rule(g, annotation, NCC_TERMINAL(AT), NCC_TERMINAL(NAME),
                  NCC_TERMINAL(LPAREN), NCC_NT(arg_list),
                  NCC_TERMINAL(RPAREN));
    ncc_add_rule(g, annotation, NCC_TERMINAL(AT), NCC_TERMINAL(NAME));

    ncc_add_rule(g, arg_list, NCC_NT(annot_arg));
    ncc_add_rule(g, arg_list, NCC_NT(annot_arg), NCC_TERMINAL(COMMA),
                  NCC_NT(arg_list));

    ncc_add_rule(g, annot_arg, NCC_TERMINAL(LITERAL));
    ncc_add_rule(g, annot_arg, NCC_TERMINAL(DOLLAR));
    ncc_add_rule(g, annot_arg, NCC_TERMINAL(NAME));
    ncc_add_rule(g, annot_arg, NCC_TERMINAL(EMPTY_LIT));

    ncc_nonterm_set_action(syntax, bnf_walk_syntax);
    ncc_nonterm_set_action(rule, bnf_walk_rule);
    ncc_nonterm_set_action(expression, bnf_walk_expression);
    ncc_nonterm_set_action(list, bnf_walk_list);
    ncc_nonterm_set_action(item, bnf_walk_item);
    ncc_nonterm_set_action(atom, bnf_walk_atom);
    ncc_nonterm_set_action(annotations, bnf_walk_annotations);
    ncc_nonterm_set_action(annotation, bnf_walk_annotation);
    ncc_nonterm_set_action(arg_list, bnf_walk_arg_list);
    ncc_nonterm_set_action(annot_arg, bnf_walk_annot_arg);

    return g;
}

// ============================================================================
// Reserved terminal -> character class mapping
// ============================================================================

static bool
reserved_to_class(ncc_string_t name, ncc_char_class_t *cc_out)
{
    if (!name.data) {
        return false;
    }

    struct {
        const char       *lit;
        ncc_char_class_t cc;
    } map[] = {
        {"__DIGIT",          NCC_CC_ASCII_DIGIT       },
        {"__ALPHA",          NCC_CC_ASCII_ALPHA        },
        {"__UPPER",          NCC_CC_ASCII_UPPER        },
        {"__LOWER",          NCC_CC_ASCII_LOWER        },
        {"__HEX",            NCC_CC_HEX_DIGIT          },
        {"__NONZERO_DIGIT",  NCC_CC_NONZERO_DIGIT      },
        {"__WHITESPACE",     NCC_CC_WHITESPACE          },
        {"__WS",             NCC_CC_WHITESPACE          },
        {"__ID_START",       NCC_CC_ID_START            },
        {"__ID_CONTINUE",    NCC_CC_ID_CONTINUE         },
        {"__PRINTABLE",      NCC_CC_PRINTABLE           },
        {"__UNICODE_DIGIT",  NCC_CC_UNICODE_DIGIT       },
        {"__JSON_STR",       NCC_CC_JSON_STRING_CHAR    },
        {"__REGEX_STR",      NCC_CC_REGEX_BODY_CHAR     },
        {nullptr,               0                          },
    };

    for (int i = 0; map[i].lit; i++) {
        if (str_eq_lit(name, map[i].lit)) {
            *cc_out = map[i].cc;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Token type name -> terminal ID mapping
// ============================================================================

static int64_t
token_type_to_id(ncc_string_t name)
{
    struct {
        const char *lit;
        int64_t     id;
    } map[] = {
        {"IDENTIFIER",   NCC_TOK_IDENTIFIER   },
        {"TYPEDEF_NAME", NCC_TOK_TYPEDEF_NAME  },
        {"INTEGER",      NCC_TOK_INTEGER       },
        {"FLOAT",        NCC_TOK_FLOAT         },
        {"CHAR",         NCC_TOK_CHAR_LIT      },
        {"STRING",       NCC_TOK_STRING_LIT    },
        {nullptr,           0                     },
    };

    for (int i = 0; map[i].lit; i++) {
        if (str_eq_lit(name, map[i].lit)) {
            return map[i].id;
        }
    }

    return 0;
}

// ============================================================================
// Convert walk results into grammar rules
// ============================================================================

static int bnf_anon_counter = 0;

static int
resolve_term_to_matches(ncc_grammar_t *user_g,
                        const char     *tagged,
                        ncc_match_t  **items_p,
                        int            *cap_p,
                        int             n)
{
    if (!tagged || strlen(tagged) < 2) {
        return n;
    }

    ncc_match_t *items = *items_p;
    int           cap   = *cap_p;
    char          type  = tagged[0];
    const char   *val   = tagged + 2;
    ncc_string_t val_s  = ncc_string_from_cstr(val);

#define ENSURE_CAP()                                                          \
    do {                                                                      \
        if (n >= cap) {                                                       \
            int old_cap = cap;                                                \
            cap = cap ? cap * 2 : 8;                                          \
            ncc_match_t *ni = ncc_alloc_array(ncc_match_t, (size_t)cap);   \
            if (items && old_cap > 0)                                         \
                memcpy(ni, items, (size_t)old_cap * sizeof(ncc_match_t));    \
            if (items) ncc_free(items);                                      \
            items = ni;                                                       \
        }                                                                     \
    } while (0)

    switch (type) {
    case 'L': {
        const char *p = val;
        while (*p) {
            int32_t cp = utf8_decode(&p);
            if (cp < 0) break;
            ENSURE_CAP();
            items[n++] = (ncc_match_t){
                .kind        = NCC_MATCH_TERMINAL,
                .terminal_id = cp,
            };
        }
        break;
    }
    case 'N': {
        int64_t ref_id = ncc_nonterm(user_g, val_s)->id;
        ENSURE_CAP();
        items[n++] = (ncc_match_t){
            .kind  = NCC_MATCH_NT,
            .nt_id = ref_id,
        };
        break;
    }
    case 'C': {
        ncc_char_class_t cc;
        if (reserved_to_class(val_s, &cc)) {
            ENSURE_CAP();
            items[n++] = (ncc_match_t){
                .kind       = NCC_MATCH_CLASS,
                .char_class = cc,
            };
        }
        break;
    }
    case 'T': {
        int64_t tok_id = token_type_to_id(val_s);
        if (tok_id) {
            ENSURE_CAP();
            items[n++] = (ncc_match_t){
                .kind        = NCC_MATCH_TERMINAL,
                .terminal_id = tok_id,
            };
        }
        break;
    }
    case 'K': {
        int64_t term_id = ncc_register_terminal(user_g, val_s);
        ENSURE_CAP();
        items[n++] = (ncc_match_t){
            .kind        = NCC_MATCH_TERMINAL,
            .terminal_id = term_id,
        };
        break;
    }
    case 'E': {
        ENSURE_CAP();
        items[n++] = (ncc_match_t){
            .kind = NCC_MATCH_EMPTY,
        };
        break;
    }
    }

#undef ENSURE_CAP

    *items_p = items;
    *cap_p   = cap;

    return n;
}

static int
resolve_terms_to_matches(ncc_grammar_t *user_g,
                         bnf_list_t     *terms,
                         ncc_match_t  **items_p,
                         int            *cap_p,
                         int             n);

static int
resolve_group_to_match(ncc_grammar_t    *user_g,
                       bnf_group_info_t  *gi,
                       ncc_match_t     **items_p,
                       int               *cap_p,
                       int                n)
{
    int min = 0, max = 0;

    switch (gi->quantifier) {
    case '?': min = 0; max = 1; break;
    case '*': min = 0; max = 0; break;
    case '+': min = 1; max = 0; break;
    default:  min = 1; max = 1; break;
    }

    ncc_match_t group_match;

    if (gi->alternatives->len == 1) {
        bnf_result_t  *alt_r       = slist_get(gi->alternatives, 0);
        bnf_list_t    *inner       = (bnf_list_t *)alt_r->data;
        ncc_match_t  *inner_items = nullptr;
        int            inner_cap   = 0;
        int            inner_n     = 0;

        inner_n = resolve_terms_to_matches(
            user_g, inner, &inner_items, &inner_cap, inner_n);

        if (inner_n > 0) {
            group_match = ncc_group_match_v(
                user_g, min, max, inner_n, inner_items);
        }
        else {
            ncc_free(inner_items);
            return n;
        }

        ncc_free(inner_items);
    }
    else {
        char namebuf[64];
        snprintf(namebuf, sizeof(namebuf),
                 "$$bnf_anon_%d", bnf_anon_counter++);
        ncc_string_t   name_s  = ncc_string_from_cstr(namebuf);
        ncc_nonterm_t *anon_nt = ncc_nonterm(user_g, name_s);
        int64_t         anon_id = anon_nt->id;

        for (size_t ai = 0; ai < gi->alternatives->len; ai++) {
            bnf_result_t  *alt_r     = slist_get(gi->alternatives, ai);
            bnf_list_t    *inner     = (bnf_list_t *)alt_r->data;
            ncc_match_t  *alt_items = nullptr;
            int            alt_cap   = 0;
            int            alt_n     = 0;

            alt_n = resolve_terms_to_matches(
                user_g, inner, &alt_items, &alt_cap, alt_n);

            if (alt_n > 0) {
                ncc_add_rule_v(user_g, anon_id, alt_n, alt_items);
            }
            else {
                ncc_match_t empty = {.kind = NCC_MATCH_EMPTY};
                ncc_add_rule_v(user_g, anon_id, 1, &empty);
            }

            ncc_free(alt_items);
        }

        ncc_match_t nt_match = {
            .kind  = NCC_MATCH_NT,
            .nt_id = anon_id,
        };

        group_match = ncc_group_match_v(user_g, min, max, 1, &nt_match);
    }

    ncc_match_t *items = *items_p;
    int           cap   = *cap_p;

    if (n >= cap) {
        int old_cap = cap;
        cap = cap ? cap * 2 : 8;
        ncc_match_t *new_items = ncc_alloc_array(ncc_match_t, (size_t)cap);
        if (items && old_cap > 0)
            memcpy(new_items, items, (size_t)old_cap * sizeof(ncc_match_t));
        if (items) ncc_free(items);
        items = new_items;
    }

    items[n++] = group_match;
    *items_p   = items;
    *cap_p     = cap;

    return n;
}

static int
resolve_terms_to_matches(ncc_grammar_t *user_g,
                         bnf_list_t     *terms,
                         ncc_match_t  **items_p,
                         int            *cap_p,
                         int             n)
{
    for (size_t ti = 0; ti < terms->len; ti++) {
        bnf_result_t *term_r = slist_get(terms, ti);

        if (term_r->tag == BNF_GROUP) {
            n = resolve_group_to_match(
                user_g, (bnf_group_info_t *)term_r->data, items_p, cap_p, n);
        }
        else {
            n = resolve_term_to_matches(
                user_g, (char *)term_r->data, items_p, cap_p, n);
        }
    }

    return n;
}

// ============================================================================
// Populate grammar from walk results
// ============================================================================

static bool
populate_grammar(ncc_grammar_t *user_g, bnf_result_t *result,
                 ncc_string_t start_symbol)
{
    if (!result || result->tag != BNF_DICT) {
        return false;
    }

    bnf_list_t *pairs = (bnf_list_t *)result->data;

    if (!pairs || !pairs->len) {
        return false;
    }

    ncc_string_t *first_name = nullptr;

    // First pass: create all non-terminals.
    for (size_t i = 0; i < pairs->len; i++) {
        bnf_result_t  *pair_r = slist_get(pairs, i);
        bnf_list_t    *pair   = (bnf_list_t *)pair_r->data;
        bnf_result_t  *name_r = slist_get(pair, 0);
        ncc_string_t *name   = (ncc_string_t *)name_r->data;

        bool is_grammar_annot = (!name || !name->data || name->data[0] == '\0');

        if (!is_grammar_annot) {
            ncc_nonterm(user_g, *name);

            if (!first_name) {
                first_name = name;
            }
        }
    }

    // Set start symbol.
    ncc_string_t start_s;

    if (start_symbol.data) {
        start_s = start_symbol;
    }
    else if (first_name) {
        start_s = *first_name;
    }
    else {
        return false;
    }

    ncc_nonterm_t *start_nt = ncc_nonterm(user_g, start_s);
    ncc_grammar_set_start(user_g, start_nt);

    // Second pass: create rules.
    for (size_t i = 0; i < pairs->len; i++) {
        bnf_result_t  *pair_r = slist_get(pairs, i);
        bnf_list_t    *pair   = (bnf_list_t *)pair_r->data;
        bnf_result_t  *name_r = slist_get(pair, 0);
        bnf_result_t  *expr_r = slist_get(pair, 1);
        ncc_string_t *name   = (ncc_string_t *)name_r->data;

        if (!name || !name->data || name->data[0] == '\0' || !expr_r) {
            continue;
        }

        int32_t penalty_cost = 0;
        bnf_result_t *annots_r = (pair->len >= 3) ? slist_get(pair, 2) : nullptr;

        if (annots_r && annots_r->tag == BNF_LIST) {
            bnf_list_t *annot_list = (bnf_list_t *)annots_r->data;

            for (size_t j = 0; j < annot_list->len; j++) {
                bnf_result_t *a_r = slist_get(annot_list, j);

                if (a_r && a_r->tag == BNF_ANNOT) {
                    bnf_annot_info_t *info = (bnf_annot_info_t *)a_r->data;

                    if (info->penalty_cost > 0) {
                        penalty_cost = info->penalty_cost;
                    }
                }
            }
        }

        int64_t nt_id = ncc_nonterm(user_g, *name)->id;

        bnf_list_t *alternatives = (bnf_list_t *)expr_r->data;

        for (size_t ai = 0; ai < alternatives->len; ai++) {
            bnf_result_t  *alt_r = slist_get(alternatives, ai);
            bnf_list_t    *terms = (bnf_list_t *)alt_r->data;
            ncc_match_t  *items = nullptr;
            int            n     = 0;
            int            cap   = 0;

            n = resolve_terms_to_matches(user_g, terms, &items, &cap, n);

            ncc_parse_rule_t *rule_p = nullptr;

            if (penalty_cost > 0) {
                if (n > 0) {
                    rule_p = ncc_add_rule_with_cost_v(
                        user_g, nt_id, penalty_cost, n, items);
                }
                else {
                    ncc_match_t empty = {.kind = NCC_MATCH_EMPTY};
                    rule_p = ncc_add_rule_with_cost_v(
                        user_g, nt_id, penalty_cost, 1, &empty);
                }

                if (rule_p) {
                    rule_p->penalty_rule = true;
                }
            }
            else {
                if (n > 0) {
                    rule_p = ncc_add_rule_v(user_g, nt_id, n, items);
                }
                else {
                    ncc_match_t empty = {.kind = NCC_MATCH_EMPTY};
                    rule_p = ncc_add_rule_v(user_g, nt_id, 1, &empty);
                }
            }

            ncc_free(items);
        }
    }

    ncc_grammar_finalize(user_g);

    return true;
}

// ============================================================================
// Free walk results recursively
// ============================================================================

static void
free_bnf_result(bnf_result_t *r)
{
    if (!r) {
        return;
    }

    switch (r->tag) {
    case BNF_STRING:
        ncc_free(r->data);
        break;

    case BNF_NAME:
        ncc_free(r->data);
        break;

    case BNF_LIST: {
        bnf_list_t *l = (bnf_list_t *)r->data;
        for (size_t i = 0; i < l->len; i++)
            free_bnf_result(slist_get(l, i));
        slist_free(l);
        break;
    }

    case BNF_PAIR: {
        bnf_list_t *pair = (bnf_list_t *)r->data;
        for (size_t i = 0; i < pair->len; i++)
            free_bnf_result(slist_get(pair, i));
        slist_free(pair);
        break;
    }

    case BNF_DICT: {
        bnf_list_t *pairs = (bnf_list_t *)r->data;
        for (size_t i = 0; i < pairs->len; i++)
            free_bnf_result(slist_get(pairs, i));
        slist_free(pairs);
        break;
    }

    case BNF_GROUP: {
        bnf_group_info_t *gi = (bnf_group_info_t *)r->data;
        if (gi->alternatives) {
            for (size_t i = 0; i < gi->alternatives->len; i++)
                free_bnf_result(slist_get(gi->alternatives, i));
            slist_free(gi->alternatives);
        }
        ncc_free(gi);
        break;
    }

    case BNF_ANNOT: {
        bnf_annot_info_t *info = (bnf_annot_info_t *)r->data;
        ncc_free(info);
        break;
    }
    }

    ncc_free(r);
}

// ============================================================================
// Public API (simplified — always uses PWZ, no _kargs)
// ============================================================================

bool
ncc_bnf_load(ncc_string_t   bnf_text,
              ncc_string_t   start_symbol,
              ncc_grammar_t *user_g)
{
    if (!bnf_text.data || !user_g) {
        return false;
    }

    // Preprocess.
    ncc_string_t stripped = ncc_bnf_strip_comments(bnf_text);
    ncc_string_t trimmed  = ncc_bnf_trim_lines(stripped);
    trimmed = bnf_join_continuations(trimmed);

    // Ensure text ends with a newline.
    size_t len = trimmed.u8_bytes;

    if (len > 0 && trimmed.data[len - 1] != '\n') {
        char *tmp = ncc_alloc_array(char, len + 2);
        memcpy(tmp, trimmed.data, len);
        tmp[len]     = '\n';
        tmp[len + 1] = '\0';
        trimmed = ncc_string_from_raw(tmp, (int64_t)(len + 1));
    }

    // Tokenize using the scanner API.
    ncc_buffer_t  *bnf_buf = ncc_buffer_from_bytes(trimmed.data,
                                                      (int64_t)trimmed.u8_bytes);
    bool            in_angle = false;
    ncc_scanner_t *bnf_sc   = ncc_scanner_new(bnf_buf, bnf_scan, nullptr,
                                                  ncc_option_none(ncc_string_t),
                                                  &in_angle, nullptr);
    ncc_token_stream_t *bnf_ts = ncc_token_stream_new(bnf_sc);
    ncc_list_t(ncc_token_info_t) bnf_tl = ncc_stream_collect(bnf_ts);

    if (ncc_list_len(bnf_tl) == 0) {
        ncc_list_free(bnf_tl);
        ncc_token_stream_free(bnf_ts);
        ncc_scanner_free(bnf_sc);
        ncc_buffer_free(bnf_buf);
        return false;
    }

    // Build token pointer array and wrap as a new stream for the parser.
    ncc_token_info_ptr_t *raw_ptrs;
    int32_t                bnf_n = ncc_token_list_build_ptrs(&bnf_tl, &raw_ptrs);

    ncc_token_stream_t *parse_ts
        = ncc_token_stream_from_array(raw_ptrs, bnf_n);

    // Build and use the BNF meta-grammar.
    ncc_grammar_t      *meta_g = build_bnf_grammar();
    ncc_parse_forest_t  forest = ncc_pwz_parse_grammar(meta_g, parse_ts);

    if (ncc_parse_forest_count(&forest) < 1) {
        ncc_parse_forest_free(&forest);
        ncc_free(raw_ptrs);
        ncc_token_stream_free(parse_ts);
        ncc_list_free(bnf_tl);
        ncc_token_stream_free(bnf_ts);
        ncc_scanner_free(bnf_sc);
        ncc_buffer_free(bnf_buf);
        ncc_grammar_free(meta_g);
        return false;
    }

    // Walk the first parse tree.
    bnf_result_t *result = (bnf_result_t *)ncc_parse_forest_walk_best(
        &forest, nullptr);

    bool success = populate_grammar(user_g, result, start_symbol);

    free_bnf_result(result);
    ncc_parse_forest_free(&forest);
    ncc_free(raw_ptrs);
    ncc_token_stream_free(parse_ts);
    ncc_list_free(bnf_tl);
    ncc_token_stream_free(bnf_ts);
    ncc_scanner_free(bnf_sc);
    ncc_buffer_free(bnf_buf);
    ncc_grammar_free(meta_g);

    return success;
}
