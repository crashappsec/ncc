// token_stream.c — Token stream with growable list, peek/rewind, save/restore,
//                   random access, and reset for re-tokenization.

#include "scanner/token_stream.h"
#include "unicode/encoding.h"
#include "lib/string.h"
#include "lib/option.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal: free trivia chains on a token
// ============================================================================

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
free_token_trivia(ncc_token_info_t *tok)
{
    free_trivia_chain(tok->leading_trivia);
    free_trivia_chain(tok->trailing_trivia);
    tok->leading_trivia  = nullptr;
    tok->trailing_trivia = nullptr;
}

// ============================================================================
// Internal: ensure token at absolute position is in the list
// ============================================================================

static bool
ensure_filled(ncc_token_stream_t *ts, int32_t target)
{
    while (ts->token_count <= target) {
        if (ts->scanner->at_eof) {
            return false;
        }

        bool got = ts->scanner->cb(ts->scanner);

        if (!got) {
            ts->scanner->at_eof = true;
            return ts->token_count > target;
        }
    }

    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

ncc_token_stream_t *
ncc_token_stream_new(ncc_scanner_t *scanner)
{
    assert(scanner);

    ncc_token_stream_t *ts = ncc_alloc(ncc_token_stream_t);

    ts->scanner     = scanner;
    ts->token_cap   = 128;
    ts->tokens      = ncc_alloc_array(ncc_token_info_t *, 128);
    ts->token_count = 0;
    ts->pos         = 0;
    ts->next_index  = 0;
    ts->exhausted   = false;

    // Wire up the back-pointer so the scanner can emit into our list.
    scanner->stream = ts;

    return ts;
}

void
ncc_token_stream_free(ncc_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    // Free trivia on all tokens.
    for (int32_t i = 0; i < ts->token_count; i++) {
        if (ts->tokens[i]) {
            free_token_trivia(ts->tokens[i]);
            ncc_free(ts->tokens[i]);
        }
    }

    ncc_free(ts->tokens);
    ncc_free(ts);
}

// ============================================================================
// Forward iteration
// ============================================================================

ncc_token_info_t *
ncc_stream_next(ncc_token_stream_t *ts)
{
    if (!ensure_filled(ts, ts->pos)) {
        ts->exhausted = true;
        return nullptr;
    }

    return ts->tokens[ts->pos++];
}

bool
ncc_stream_is_done(ncc_token_stream_t *ts)
{
    if (ts->exhausted) {
        return true;
    }

    if (!ensure_filled(ts, ts->pos)) {
        ts->exhausted = true;
        return true;
    }

    return false;
}

// ============================================================================
// Peek (lookahead)
// ============================================================================

ncc_token_info_t *
ncc_stream_peek(ncc_token_stream_t *ts, int32_t n)
{
    int32_t target = ts->pos + n;

    if (target < 0) {
        return nullptr;
    }

    if (!ensure_filled(ts, target)) {
        return nullptr;
    }

    return ts->tokens[target];
}

// ============================================================================
// Rewind (lookback)
// ============================================================================

bool
ncc_stream_rewind(ncc_token_stream_t *ts, int32_t n)
{
    int32_t new_pos = ts->pos - n;

    if (new_pos < 0) {
        return false;
    }

    ts->pos       = new_pos;
    ts->exhausted = false;

    return true;
}

ncc_token_info_t *
ncc_stream_lookback(ncc_token_stream_t *ts, int32_t n)
{
    int32_t target = ts->pos - n;

    if (target < 0 || target >= ts->token_count) {
        return nullptr;
    }

    return ts->tokens[target];
}

// ============================================================================
// Random access
// ============================================================================

ncc_token_info_t *
ncc_stream_get(ncc_token_stream_t *ts, int32_t pos)
{
    if (pos < 0) {
        return nullptr;
    }

    if (!ensure_filled(ts, pos)) {
        return nullptr;
    }

    return ts->tokens[pos];
}

int32_t
ncc_stream_token_count(ncc_token_stream_t *ts)
{
    return ts->token_count;
}

// ============================================================================
// Reset (re-tokenize from scratch)
// ============================================================================

void
ncc_stream_reset(ncc_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    // Free all token structs.
    for (int32_t i = 0; i < ts->token_count; i++) {
        if (ts->tokens[i]) {
            free_token_trivia(ts->tokens[i]);
            ncc_free(ts->tokens[i]);
            ts->tokens[i] = nullptr;
        }
    }

    ts->token_count = 0;
    ts->pos         = 0;
    ts->next_index  = 0;
    ts->exhausted   = false;

    // Reset the scanner to re-tokenize from the beginning.
    ncc_scanner_reset(ts->scanner);
}

// ============================================================================
// Save / restore
// ============================================================================

ncc_stream_mark_t
ncc_stream_save(ncc_token_stream_t *ts)
{
    return (ncc_stream_mark_t){ .pos = ts->pos };
}

bool
ncc_stream_restore(ncc_token_stream_t *ts, ncc_stream_mark_t mark)
{
    if (mark.pos < 0 || mark.pos > ts->token_count) {
        return false;
    }

    ts->pos       = mark.pos;
    ts->exhausted = false;

    return true;
}

// ============================================================================
// Collect all
// ============================================================================

ncc_list_t(ncc_token_info_t)
ncc_stream_collect(ncc_token_stream_t *ts)
{
    ncc_list_t(ncc_token_info_t) tl = ncc_list_new_cap(ncc_token_info_t, 64);

    ncc_token_info_t *tok;

    while ((tok = ncc_stream_next(ts)) != nullptr) {
        ncc_list_push(tl, *tok);
    }

    return tl;
}

// ============================================================================
// Array-backed stream
// ============================================================================

typedef struct {
    ncc_token_info_t **tokens;
    int32_t             count;
    int32_t             pos;
} array_scan_state_t;

static bool
array_scan_cb(ncc_scanner_t *s)
{
    array_scan_state_t *st = (array_scan_state_t *)s->user_state;

    if (st->pos >= st->count) {
        return false;
    }

    ncc_token_info_t *src = st->tokens[st->pos];

    // Emit the token into the stream's growable list.
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

    // Copy the token so the stream owns the memory.
    ncc_token_info_t *copy = ncc_alloc(ncc_token_info_t);
    *copy = *src;
    copy->index = ts->next_index++;

    ts->tokens[ts->token_count++] = copy;

    st->pos++;
    return true;
}

static void
array_scan_reset_cb(ncc_scanner_t *s)
{
    array_scan_state_t *st = (array_scan_state_t *)s->user_state;
    st->pos = 0;
}

ncc_token_stream_t *
ncc_token_stream_from_array(ncc_token_info_t **tokens, int32_t count)
{
    array_scan_state_t *st = ncc_alloc(array_scan_state_t);

    st->tokens = tokens;
    st->count  = count;
    st->pos    = 0;

    // Create a dummy buffer — the scanner callback doesn't use input bytes.
    ncc_buffer_t *dummy_buf = ncc_buffer_empty();

    ncc_scanner_t *sc = ncc_scanner_new(dummy_buf, array_scan_cb, nullptr,
                                            ncc_option_none(ncc_string_t),
                                            st,
                                            array_scan_reset_cb);

    return ncc_token_stream_new(sc);
}

// ============================================================================
// Codepoint-level stream
// ============================================================================

typedef struct {
    const char *data;       // UTF-8 bytes
    uint32_t    byte_len;   // Total byte count
    uint32_t    byte_pos;   // Current byte offset
    int32_t     tok_index;  // Token index counter
    uint32_t    line;       // Current line number
    uint32_t    column;     // Current column number
} cp_scan_state_t;

static void
cp_scan_emit(ncc_token_stream_t *ts, ncc_token_info_t *tok)
{
    if (ts->token_count >= ts->token_cap) {
        int32_t new_cap = ts->token_cap ? ts->token_cap * 2 : 128;
        ncc_token_info_t **new_tokens
            = ncc_alloc_array(ncc_token_info_t *, new_cap);

        if (ts->tokens && ts->token_count > 0) {
            memcpy(new_tokens, ts->tokens,
                   (size_t)ts->token_count * sizeof(ncc_token_info_t *));
        }

        ncc_free(ts->tokens);
        ts->tokens    = new_tokens;
        ts->token_cap = new_cap;
    }

    ts->tokens[ts->token_count++] = tok;
}

static bool
cp_scan_cb(ncc_scanner_t *s)
{
    cp_scan_state_t     *st = (cp_scan_state_t *)s->user_state;
    ncc_token_stream_t *ts = s->stream;

    if (st->byte_pos >= st->byte_len) {
        return false;
    }

    uint32_t start_pos = st->byte_pos;
    int32_t  cp = ncc_unicode_utf8_decode(st->data, st->byte_len,
                                            &st->byte_pos);

    if (cp < 0) {
        // Invalid UTF-8 — skip byte and retry.
        st->byte_pos = start_pos + 1;
        st->column++;
        return true;
    }

    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);
    tok->tid    = (int32_t)cp;
    tok->index  = ts->next_index++;
    tok->line   = st->line;
    tok->column = st->column;

    // Set value to the one-codepoint string.
    uint32_t cp_byte_len = st->byte_pos - start_pos;
    ncc_string_t val = ncc_string_from_raw(st->data + start_pos,
                                               (int64_t)cp_byte_len);
    tok->value = ncc_option_set(ncc_string_t, val);

    cp_scan_emit(ts, tok);

    // Track line/column.
    if (cp == '\n') {
        st->line++;
        st->column = 1;
    }
    else {
        st->column++;
    }

    return true;
}

static void
cp_scan_reset_cb(ncc_scanner_t *s)
{
    cp_scan_state_t *st = (cp_scan_state_t *)s->user_state;
    st->byte_pos  = 0;
    st->tok_index = 0;
    st->line      = 1;
    st->column    = 1;
}

ncc_token_stream_t *
ncc_token_stream_from_codepoints(ncc_string_t input)
{
    cp_scan_state_t *st = ncc_alloc(cp_scan_state_t);

    st->data      = input.data;
    st->byte_len  = (uint32_t)input.u8_bytes;
    st->byte_pos  = 0;
    st->tok_index = 0;
    st->line      = 1;
    st->column    = 1;

    ncc_buffer_t *dummy_buf = ncc_buffer_empty();

    ncc_scanner_t *sc = ncc_scanner_new(dummy_buf, cp_scan_cb, nullptr,
                                            ncc_option_none(ncc_string_t),
                                            st,
                                            cp_scan_reset_cb);

    return ncc_token_stream_new(sc);
}
