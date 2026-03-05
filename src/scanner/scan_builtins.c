// scan_recipes.c — Higher-level scanning recipes for common token patterns.

#include "scanner/scan_builtins.h"
#include "lib/buffer.h"
#include "unicode/encoding.h"
#include <assert.h>
#include <string.h>

// ============================================================================
// Internal: hex digit value
// ============================================================================

static int
hex_val(ncc_codepoint_t cp)
{
    if (cp >= '0' && cp <= '9') return (int)(cp - '0');
    if (cp >= 'a' && cp <= 'f') return (int)(cp - 'a' + 10);
    if (cp >= 'A' && cp <= 'F') return (int)(cp - 'A' + 10);
    return -1;
}

// ============================================================================
// Internal: string builder helpers wrapping ncc_buffer_t
// ============================================================================

static void
sb_push_cp(ncc_buffer_t *sb, ncc_codepoint_t cp)
{
    char enc[4];
    uint32_t n = ncc_unicode_utf8_encode(cp, enc);

    ncc_buffer_append(sb, enc, n);
}

static ncc_option_t(ncc_string_t)
sb_finish(ncc_buffer_t *sb)
{
    ncc_string_t str = ncc_string_from_raw(sb->data, (int64_t)sb->byte_len);
    ncc_free(sb->data);
    ncc_free(sb);

    return ncc_option_set(ncc_string_t, str);
}

static void
sb_discard(ncc_buffer_t *sb)
{
    ncc_buffer_free(sb);
}

// ============================================================================
// Internal: scan N hex digits into a codepoint
// ============================================================================

static bool
scan_hex_digits(ncc_scanner_t *s, int count, ncc_codepoint_t *out)
{
    ncc_codepoint_t val = 0;

    for (int i = 0; i < count; i++) {
        ncc_codepoint_t cp = ncc_scan_peek(s, 0);
        int hv              = hex_val(cp);

        if (hv < 0) {
            return false;
        }

        val = (val << 4) | (ncc_codepoint_t)hv;
        ncc_scan_advance(s);
    }

    *out = val;
    return true;
}

// ============================================================================
// Internal: quoted string with escape processing
// ============================================================================

static ncc_option_t(ncc_string_t)
scan_quoted_string(ncc_scanner_t *s, ncc_codepoint_t quote_cp)
{
    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    if (cp != quote_cp) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);
    ncc_scan_advance(s);  // Skip opening quote.

    ncc_buffer_t *sb = ncc_buffer_empty();

    while (!ncc_scan_at_eof(s)) {
        cp = ncc_scan_peek(s, 0);

        if (cp == quote_cp) {
            ncc_scan_advance(s);  // Skip closing quote.
            return sb_finish(sb);
        }

        if (cp == '\\') {
            ncc_scan_advance(s);  // Skip backslash.

            if (ncc_scan_at_eof(s)) {
                sb_discard(sb);
                return ncc_option_none(ncc_string_t);
            }

            cp = ncc_scan_peek(s, 0);
            ncc_scan_advance(s);

            switch (cp) {
            case '\\': ncc_buffer_putc(sb,'\\'); break;
            case 'n':  ncc_buffer_putc(sb,'\n'); break;
            case 't':  ncc_buffer_putc(sb,'\t'); break;
            case 'r':  ncc_buffer_putc(sb,'\r'); break;
            case '0':  ncc_buffer_putc(sb,'\0'); break;
            case '\'': ncc_buffer_putc(sb,'\''); break;
            case '"':  ncc_buffer_putc(sb,'"');  break;
            case 'x': {
                ncc_codepoint_t val;
                if (!scan_hex_digits(s, 2, &val)) {
                    sb_discard(sb);
                    return ncc_option_none(ncc_string_t);
                }
                ncc_buffer_putc(sb,(char)(uint8_t)val);
                break;
            }
            case 'u': {
                ncc_codepoint_t val;
                if (!scan_hex_digits(s, 4, &val)) {
                    sb_discard(sb);
                    return ncc_option_none(ncc_string_t);
                }
                sb_push_cp(sb,val);
                break;
            }
            case 'U': {
                ncc_codepoint_t val;
                if (!scan_hex_digits(s, 8, &val)) {
                    sb_discard(sb);
                    return ncc_option_none(ncc_string_t);
                }
                sb_push_cp(sb,val);
                break;
            }
            default:
                ncc_buffer_putc(sb,'\\');
                sb_push_cp(sb,cp);
                break;
            }
        }
        else {
            size_t before = ncc_scan_offset(s);

            ncc_scan_advance(s);

            size_t after  = ncc_scan_offset(s);
            size_t nbytes = after - before;

            // Push raw bytes for a single codepoint.
            ncc_buffer_append(sb, s->input + before, nbytes);
        }
    }

    // Unterminated string.
    sb_discard(sb);
    return ncc_option_none(ncc_string_t);
}

// ============================================================================
// String recipes
// ============================================================================

ncc_option_t(ncc_string_t)
ncc_scan_string_double(ncc_scanner_t *s)
{
    return scan_quoted_string(s, '"');
}

ncc_option_t(ncc_string_t)
ncc_scan_string_single(ncc_scanner_t *s)
{
    return scan_quoted_string(s, '\'');
}

ncc_option_t(ncc_string_t)
ncc_scan_string_raw(ncc_scanner_t *s, const char *quote)
{
    size_t qlen = strlen(quote);

    if (s->cursor + qlen > s->input_len
        || memcmp(s->input + s->cursor, quote, qlen) != 0) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);
    ncc_scan_advance_bytes(s, qlen);

    size_t content_start = s->cursor;

    while (s->cursor + qlen <= s->input_len) {
        if (memcmp(s->input + s->cursor, quote, qlen) == 0) {
            size_t  content_end = s->cursor;
            size_t  len         = content_end - content_start;

            ncc_string_t str = ncc_string_from_raw(s->input + content_start,
                                                      (int64_t)len);

            ncc_scan_advance_bytes(s, qlen);
            return ncc_option_set(ncc_string_t, str);
        }

        ncc_scan_advance(s);
    }

    // Unterminated.
    return ncc_option_none(ncc_string_t);
}

// ============================================================================
// Number recipes
// ============================================================================

static bool
is_digit(ncc_codepoint_t cp)
{
    return cp >= '0' && cp <= '9';
}

static bool
is_hex(ncc_codepoint_t cp)
{
    return (cp >= '0' && cp <= '9')
        || (cp >= 'a' && cp <= 'f')
        || (cp >= 'A' && cp <= 'F');
}

static bool
is_binary(ncc_codepoint_t cp)
{
    return cp == '0' || cp == '1';
}

static bool
is_octal(ncc_codepoint_t cp)
{
    return cp >= '0' && cp <= '7';
}

static int32_t
skip_digits(ncc_scanner_t *s, bool (*pred)(ncc_codepoint_t))
{
    int32_t count = 0;

    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t cp = ncc_scan_peek(s, 0);

        if (cp == '_') {
            ncc_scan_advance(s);
            continue;
        }

        if (!pred(cp)) {
            break;
        }

        ncc_scan_advance(s);
        count++;
    }

    return count;
}

ncc_option_t(ncc_string_t)
ncc_scan_integer(ncc_scanner_t *s)
{
    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);

    if (cp == '0') {
        ncc_codepoint_t next = ncc_scan_peek(s, 1);

        if (next == 'x' || next == 'X') {
            ncc_scan_advance(s);
            ncc_scan_advance(s);

            if (skip_digits(s, is_hex) == 0) {
                return ncc_option_none(ncc_string_t);
            }

            return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
        }

        if (next == 'o' || next == 'O') {
            ncc_scan_advance(s);
            ncc_scan_advance(s);

            if (skip_digits(s, is_octal) == 0) {
                return ncc_option_none(ncc_string_t);
            }

            return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
        }

        if (next == 'b' || next == 'B') {
            ncc_scan_advance(s);
            ncc_scan_advance(s);

            if (skip_digits(s, is_binary) == 0) {
                return ncc_option_none(ncc_string_t);
            }

            return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
        }
    }

    skip_digits(s, is_digit);
    return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
}

ncc_option_t(ncc_string_t)
ncc_scan_float(ncc_scanner_t *s)
{
    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);
    skip_digits(s, is_digit);

    bool has_dot      = false;
    bool has_exponent = false;

    if (ncc_scan_peek(s, 0) == '.') {
        ncc_codepoint_t after_dot = ncc_scan_peek(s, 1);

        if (is_digit(after_dot)) {
            has_dot = true;
            ncc_scan_advance(s);
            skip_digits(s, is_digit);
        }
    }

    cp = ncc_scan_peek(s, 0);

    if (cp == 'e' || cp == 'E') {
        has_exponent = true;
        ncc_scan_advance(s);

        cp = ncc_scan_peek(s, 0);

        if (cp == '+' || cp == '-') {
            ncc_scan_advance(s);
        }

        if (skip_digits(s, is_digit) == 0) {
            return ncc_option_none(ncc_string_t);
        }
    }

    if (!has_dot && !has_exponent) {
        return ncc_option_none(ncc_string_t);
    }

    return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
}

bool
ncc_scan_number(ncc_scanner_t *s, int32_t int_tid, int32_t float_tid)
{
    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    if (!is_digit(cp)) {
        return false;
    }

    // Save position to try float first, then fallback to integer.
    size_t   saved_cursor = s->cursor;
    uint32_t saved_line   = s->line;
    uint32_t saved_col    = s->column;

    // Try float.
    ncc_scan_mark(s);

    ncc_option_t(ncc_string_t) fval = ncc_scan_float(s);

    if (ncc_option_is_set(fval)) {
        ncc_scan_emit(s, float_tid, fval);
        return true;
    }

    // Restore and try integer.
    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    ncc_scan_mark(s);

    ncc_option_t(ncc_string_t) ival = ncc_scan_integer(s);

    if (ncc_option_is_set(ival)) {
        ncc_scan_emit(s, int_tid, ival);
        return true;
    }

    // Restore cursor on failure.
    s->cursor = saved_cursor;
    s->line   = saved_line;
    s->column = saved_col;

    return false;
}

// ============================================================================
// Identifier recipe
// ============================================================================

ncc_option_t(ncc_string_t)
ncc_scan_identifier(ncc_scanner_t *s)
{
    if (ncc_scan_at_eof(s)) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_codepoint_t cp = ncc_scan_peek(s, 0);

    if (!ncc_unicode_is_id_start(cp)) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);
    ncc_scan_advance(s);

    while (!ncc_scan_at_eof(s)) {
        cp = ncc_scan_peek(s, 0);

        if (!ncc_unicode_is_id_continue(cp)) {
            break;
        }

        ncc_scan_advance(s);
    }

    return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
}
