// scanner.c — Streaming tokenizer scanner implementation.

#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"
#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: UTF-8 decode at position
// ============================================================================

static ncc_codepoint_t
decode_at(ncc_scanner_t *s, size_t off, int *cp_len)
{
    if (off >= s->input_len) {
        *cp_len = 0;
        return 0;
    }

    uint32_t pos = (uint32_t)off;
    int32_t  cp  = ncc_unicode_utf8_decode(s->input, (uint32_t)s->input_len, &pos);

    if (cp < 0) {
        *cp_len = 1;
        return (ncc_codepoint_t)(uint8_t)s->input[off];
    }

    *cp_len = (int)(pos - (uint32_t)off);
    return (ncc_codepoint_t)cp;
}

// ============================================================================
// Internal: character class matching
// ============================================================================

static bool
cc_match(ncc_char_class_t cc, ncc_codepoint_t cp)
{
    switch (cc) {
    case NCC_CC_ID_START:
        return ncc_unicode_is_id_start(cp);
    case NCC_CC_ID_CONTINUE:
        return ncc_unicode_is_id_continue(cp);
    case NCC_CC_ASCII_DIGIT:
        return cp >= '0' && cp <= '9';
    case NCC_CC_UNICODE_DIGIT: {
        ncc_unicode_gc_t gc = ncc_unicode_general_category(cp);
        return gc == NCC_UNICODE_GC_ND;
    }
    case NCC_CC_ASCII_UPPER:
        return cp >= 'A' && cp <= 'Z';
    case NCC_CC_ASCII_LOWER:
        return cp >= 'a' && cp <= 'z';
    case NCC_CC_ASCII_ALPHA:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case NCC_CC_WHITESPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v'
            || cp == 0x85   // NEL
            || cp == 0xA0   // NBSP
            || cp == 0x2028 // Line separator
            || cp == 0x2029; // Paragraph separator
    case NCC_CC_HEX_DIGIT:
        return (cp >= '0' && cp <= '9')
            || (cp >= 'a' && cp <= 'f')
            || (cp >= 'A' && cp <= 'F');
    case NCC_CC_NONZERO_DIGIT:
        return cp >= '1' && cp <= '9';
    case NCC_CC_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F;
    case NCC_CC_NON_WS_PRINTABLE:
        return cp > 0x20 && cp != 0x7F;
    case NCC_CC_NON_NL_WS:
        return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f' || cp == '\v';
    case NCC_CC_NON_NL_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F && cp != '\n';
    case NCC_CC_JSON_STRING_CHAR:
        return cp >= 0x20 && cp != '"' && cp != '\\';
    case NCC_CC_REGEX_BODY_CHAR:
        return cp >= 0x20 && cp != '/' && cp != '\\';
    }
    return false;
}

// ============================================================================
// Internal: trivia allocation
// ============================================================================

static ncc_trivia_t *
make_trivia(ncc_string_t text)
{
    ncc_trivia_t *t = ncc_alloc(ncc_trivia_t);

    t->text = text;
    t->next = nullptr;
    return t;
}

static void
free_trivia_chain(ncc_trivia_t *t)
{
    while (t) {
        ncc_trivia_t *next = t->next;
        // t->text.data is GC-managed; just free the node.
        ncc_free(t);
        t = next;
    }
}

static void
append_trivia(ncc_trivia_t **head, ncc_trivia_t *node)
{
    if (!*head) {
        *head = node;
        return;
    }

    ncc_trivia_t *tail = *head;

    while (tail->next) {
        tail = tail->next;
    }

    tail->next = node;
}

// ============================================================================
// Scanner lifecycle
// ============================================================================

ncc_scanner_t *
ncc_scanner_new(ncc_buffer_t                *buf,
                 ncc_scan_cb_t                cb,
                 ncc_grammar_t               *grammar,
                 ncc_option_t(ncc_string_t)  file,
                 void                         *state,
                 ncc_scan_reset_cb_t          reset_cb)
{
    assert(buf);
    assert(cb);

    ncc_scanner_t *s = ncc_alloc(ncc_scanner_t);

    s->input     = buf->data;
    s->input_len = buf->byte_len;
    s->cursor    = 0;
    s->line      = 1;
    s->column    = 1;
    s->file      = file;
    s->cb        = cb;
    s->reset_cb  = reset_cb;
    s->user_state = state;
    s->grammar   = grammar;
    s->at_eof    = false;

    // Mark defaults to start of input.
    s->mark      = 0;
    s->mark_line = 1;
    s->mark_col  = 1;

    // Terminal ID cache is lazily initialized.
    s->terminal_ids = nullptr;

    return s;
}

void
ncc_scanner_free(ncc_scanner_t *s)
{
    if (!s) {
        return;
    }

    // Free pending trivia nodes (text data is GC-managed).
    free_trivia_chain(s->pending_leading);
    free_trivia_chain(s->pending_trailing);

    // The terminal_ids dict is GC-managed (ncc_alloc); no explicit free needed.

    ncc_free(s);
}

void
ncc_scanner_reset(ncc_scanner_t *s)
{
    if (!s) {
        return;
    }

    s->cursor   = 0;
    s->line     = 1;
    s->column   = 1;
    s->mark     = 0;
    s->mark_line = 1;
    s->mark_col  = 1;
    s->at_eof   = false;

    // Free pending trivia.
    free_trivia_chain(s->pending_leading);
    free_trivia_chain(s->pending_trailing);
    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    // Reset user state if callback is set.
    if (s->reset_cb) {
        s->reset_cb(s);
    }
}

// ============================================================================
// Cursor inspection
// ============================================================================

ncc_codepoint_t
ncc_scan_peek(ncc_scanner_t *s, int32_t offset)
{
    // Guard against negative offsets that would underflow.
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;
    int    cp_len;

    return decode_at(s, pos, &cp_len);
}

uint8_t
ncc_scan_peek_byte(ncc_scanner_t *s, int32_t offset)
{
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;

    if (pos >= s->input_len) {
        return 0;
    }

    return (uint8_t)s->input[pos];
}

bool
ncc_scan_at_eof(ncc_scanner_t *s)
{
    return s->cursor >= s->input_len;
}

size_t
ncc_scan_remaining(ncc_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    return s->input_len - s->cursor;
}

size_t
ncc_scan_offset(ncc_scanner_t *s)
{
    return s->cursor;
}

// ============================================================================
// Cursor movement
// ============================================================================

void
ncc_scan_advance(ncc_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return;
    }

    int              cp_len;
    ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }
}

void
ncc_scan_advance_n(ncc_scanner_t *s, int32_t n)
{
    for (int32_t i = 0; i < n && s->cursor < s->input_len; i++) {
        ncc_scan_advance(s);
    }
}

void
ncc_scan_advance_bytes(ncc_scanner_t *s, size_t n)
{
    size_t end = s->cursor + n;

    if (end > s->input_len) {
        end = s->input_len;
    }

    size_t delta = end - s->cursor;

    s->cursor = end;
    s->column += (uint32_t)delta;
}

// ============================================================================
// Matching helpers
// ============================================================================

size_t
ncc_scan_match_str(ncc_scanner_t *s, const char *lit)
{
    size_t len = strlen(lit);

    if (s->cursor + len > s->input_len) {
        return 0;
    }

    if (memcmp(s->input + s->cursor, lit, len) != 0) {
        return 0;
    }

    for (size_t i = 0; i < len; ) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    return len;
}

size_t
ncc_scan_match_if(ncc_scanner_t *s, ncc_cp_predicate_fn pred)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!pred(cp, nullptr)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
ncc_scan_match_class(ncc_scanner_t *s, ncc_char_class_t cc)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!cc_match(cc, cp)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
ncc_scan_match_cp(ncc_scanner_t *s, ncc_codepoint_t target)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (cp != target) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

int32_t
ncc_scan_skip_while(ncc_scanner_t *s, ncc_cp_predicate_fn pred)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!pred(cp, nullptr)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

int32_t
ncc_scan_skip_class(ncc_scanner_t *s, ncc_char_class_t cc)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(cc, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

size_t
ncc_scan_skip_until_cp(ncc_scanner_t *s, ncc_codepoint_t stop)
{
    size_t start = s->cursor;

    while (s->cursor < s->input_len) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (cp == stop) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    return s->cursor - start;
}

ncc_result_t(size_t)
ncc_scan_skip_until_str(ncc_scanner_t *s, const char *needle)
{
    size_t needle_len = strlen(needle);
    size_t start      = s->cursor;

    while (s->cursor + needle_len <= s->input_len) {
        if (memcmp(s->input + s->cursor, needle, needle_len) == 0) {
            return ncc_result_ok(size_t, s->cursor - start);
        }

        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Needle not found; advance to end.
    while (s->cursor < s->input_len) {
        ncc_scan_advance(s);
    }

    return ncc_result_err(size_t, NCC_ERR_SCAN_NOT_FOUND);
}

// ============================================================================
// Mark / extract
// ============================================================================

void
ncc_scan_mark(ncc_scanner_t *s)
{
    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

ncc_string_t
ncc_scan_extract(ncc_scanner_t *s)
{
    size_t len = s->cursor - s->mark;

    return ncc_string_from_raw(s->input + s->mark, (int64_t)len);
}

size_t
ncc_scan_mark_len(ncc_scanner_t *s)
{
    return s->cursor - s->mark;
}

// ============================================================================
// Token emission
// ============================================================================

void
ncc_scan_emit(ncc_scanner_t *s, int32_t tid, ncc_option_t(ncc_string_t) value)
{
    assert(s->stream);

    ncc_token_stream_t *ts = s->stream;

    // Grow token array if needed.
    if (ts->token_count >= ts->token_cap) {
        int32_t new_cap = ts->token_cap ? ts->token_cap * 2 : 128;
        ncc_token_info_t **new_tokens = ncc_alloc_array(ncc_token_info_t *, new_cap);

        if (ts->tokens && ts->token_count > 0) {
            memcpy(new_tokens, ts->tokens,
                   (size_t)ts->token_count * sizeof(ncc_token_info_t *));
        }

        ncc_free(ts->tokens);
        ts->tokens    = new_tokens;
        ts->token_cap = new_cap;
    }

    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);
    memset(tok, 0, sizeof(*tok));

    tok->tid             = tid;
    tok->value           = value;
    tok->file            = s->file;
    tok->line            = s->mark_line;
    tok->column          = s->mark_col;
    tok->endcol          = s->column;
    tok->index           = ts->next_index++;
    tok->leading_trivia  = s->pending_leading;
    tok->trailing_trivia = s->pending_trailing;

    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    ts->tokens[ts->token_count++] = tok;

    // Reset mark to current cursor.
    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

void
ncc_scan_emit_marked(ncc_scanner_t *s, int32_t tid)
{
    ncc_string_t val = ncc_scan_extract(s);

    ncc_scan_emit(s, tid, ncc_option_set(ncc_string_t, val));
}

// ============================================================================
// Trivia helpers
// ============================================================================

void
ncc_scan_add_leading_trivia(ncc_scanner_t *s, ncc_string_t text)
{
    ncc_trivia_t *t = make_trivia(text);

    append_trivia(&s->pending_leading, t);
}

void
ncc_scan_add_trailing_trivia(ncc_scanner_t *s, ncc_string_t text)
{
    ncc_trivia_t *t = make_trivia(text);

    append_trivia(&s->pending_trailing, t);
}

int32_t
ncc_scan_skip_whitespace(ncc_scanner_t *s)
{
    size_t  start = s->cursor;
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(NCC_CC_WHITESPACE, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    if (count > 0) {
        size_t byte_len = s->cursor - start;

        ncc_string_t text = ncc_string_from_raw(s->input + start,
                                                   (int64_t)byte_len);
        ncc_scan_add_leading_trivia(s, text);
    }

    return count;
}

void
ncc_scan_skip_line_comment(ncc_scanner_t *s)
{
    size_t start = s->cursor;

    while (s->cursor < s->input_len) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (cp == '\n') {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->column++;
    }

    size_t byte_len = s->cursor - start;

    if (byte_len > 0) {
        ncc_string_t text = ncc_string_from_raw(s->input + start,
                                                   (int64_t)byte_len);
        ncc_scan_add_trailing_trivia(s, text);
    }
}

bool
ncc_scan_skip_block_comment(ncc_scanner_t *s,
                              const char     *opener,
                              const char     *closer)
{
    size_t opener_len = strlen(opener);
    size_t closer_len = strlen(closer);
    size_t start      = s->cursor;

    // Skip past opener.
    if (s->cursor + opener_len > s->input_len
        || memcmp(s->input + s->cursor, opener, opener_len) != 0) {
        return false;
    }

    for (size_t i = 0; i < opener_len; ) {
        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Scan for closer.
    while (s->cursor + closer_len <= s->input_len) {
        if (memcmp(s->input + s->cursor, closer, closer_len) == 0) {
            // Skip past closer.
            for (size_t i = 0; i < closer_len; ) {
                int              cp_len;
                ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

                s->cursor += (size_t)cp_len;
                i += (size_t)cp_len;

                if (cp == '\n') {
                    s->line++;
                    s->column = 1;
                }
                else {
                    s->column++;
                }
            }

            size_t byte_len = s->cursor - start;

            ncc_string_t text = ncc_string_from_raw(s->input + start,
                                                       (int64_t)byte_len);
            ncc_scan_add_leading_trivia(s, text);
            return true;
        }

        int              cp_len;
        ncc_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    // Unterminated — consume the rest, still collect as trivia.
    while (s->cursor < s->input_len) {
        ncc_scan_advance(s);
    }

    size_t byte_len = s->cursor - start;

    ncc_string_t text = ncc_string_from_raw(s->input + start,
                                               (int64_t)byte_len);
    ncc_scan_add_leading_trivia(s, text);
    return false;
}

// ============================================================================
// Grammar integration
// ============================================================================

int64_t
ncc_scan_terminal_id(ncc_scanner_t *s, const char *name)
{
    if (!s->grammar) {
        return NCC_TOK_OTHER;
    }

    // Lazily create the terminal ID dict.
    if (!s->terminal_ids) {
        s->terminal_ids = ncc_alloc(ncc_dict_t);
        ncc_dict_init(s->terminal_ids, ncc_hash_cstring, ncc_dict_cstr_eq);
    }

    // Look up in cache.
    bool found;
    void *val = ncc_dict_get(s->terminal_ids, (void *)name, &found);

    if (found) {
        return (int64_t)(intptr_t)val;
    }

    // Look up in grammar's terminal_map.
    if (!s->grammar->terminal_map) {
        return NCC_TOK_OTHER;
    }

    bool  gfound = false;
    void *gval   = _ncc_dict_get(s->grammar->terminal_map,
                                           (void *)name, &gfound);

    if (!gfound) {
        // Cache the miss so we don't re-lookup.
        _ncc_dict_put(s->terminal_ids,
                               (void *)name,
                               (void *)(intptr_t)NCC_TOK_OTHER);
        return NCC_TOK_OTHER;
    }

    int64_t tid = (int64_t)(intptr_t)gval;

    // Cache the hit.
    _ncc_dict_put(s->terminal_ids,
                           (void *)name,
                           (void *)(intptr_t)tid);
    return tid;
}
