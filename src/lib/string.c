// string.c — ncc_string_t implementations.

#include "lib/string.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

ncc_string_t
ncc_string_from_raw(const char *src, int64_t byte_len)
{
    ncc_string_t s = {0};

    if (!src || byte_len <= 0) {
        return s;
    }

    s.data = (char *)ncc_alloc_size(1, (size_t)byte_len + 1);
    memcpy(s.data, src, (size_t)byte_len);
    s.u8_bytes   = (size_t)byte_len;
    s.codepoints = (size_t)byte_len; // Approximate; fine for parser.
    s.styling    = nullptr;

    return s;
}

ncc_string_t
ncc_string_from_cstr(const char *src)
{
    ncc_string_t s = {0};

    if (!src) {
        return s;
    }

    size_t len = strlen(src);

    s.data = (char *)ncc_alloc_size(1, len + 1);
    memcpy(s.data, src, len);
    s.u8_bytes   = len;
    s.codepoints = len; // Approximate.
    s.styling    = nullptr;

    return s;
}

ncc_string_t
ncc_string_empty(void)
{
    ncc_string_t s = {0};

    s.data       = (char *)ncc_alloc_size(1, 1);
    s.u8_bytes   = 0;
    s.codepoints = 0;
    s.styling    = nullptr;

    return s;
}

bool
ncc_string_eq(ncc_string_t a, ncc_string_t b)
{
    if (a.u8_bytes != b.u8_bytes) {
        return false;
    }

    if (!a.data && !b.data) {
        return true;
    }

    if (!a.data || !b.data) {
        return false;
    }

    return memcmp(a.data, b.data, a.u8_bytes) == 0;
}

// ── ncc_string_to_ansi ──────────────────────────────────────────────────────

static void
emit_sgr(ncc_buffer_t *buf, const ncc_text_style_t *style)
{
    ncc_buffer_puts(buf, "\x1b[0");

    if (style->bold == NCC_TRI_YES)             ncc_buffer_puts(buf, ";1");
    if (style->dim == NCC_TRI_YES)              ncc_buffer_puts(buf, ";2");
    if (style->italic == NCC_TRI_YES)           ncc_buffer_puts(buf, ";3");
    if (style->underline == NCC_TRI_YES)        ncc_buffer_puts(buf, ";4");
    if (style->blink == NCC_TRI_YES)            ncc_buffer_puts(buf, ";5");
    if (style->reverse == NCC_TRI_YES)          ncc_buffer_puts(buf, ";7");
    if (style->strikethrough == NCC_TRI_YES)    ncc_buffer_puts(buf, ";9");
    if (style->double_underline == NCC_TRI_YES) ncc_buffer_puts(buf, ";21");

    if (ncc_color_is_set(style->fg_rgb)) {
        int  rgb = ncc_color_rgb(style->fg_rgb);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ";38;2;%d;%d;%d",
                 (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        ncc_buffer_puts(buf, tmp);
    }

    if (ncc_color_is_set(style->bg_rgb)) {
        int  rgb = ncc_color_rgb(style->bg_rgb);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), ";48;2;%d;%d;%d",
                 (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        ncc_buffer_puts(buf, tmp);
    }

    ncc_buffer_putc(buf, 'm');
}

char *
ncc_string_to_ansi(const ncc_string_t *s)
{
    if (!s) {
        return nullptr;
    }

    if (!s->styling) {
        return strdup(s->data);
    }

    ncc_string_style_info_t *si  = (ncc_string_style_info_t *)s->styling;
    ncc_buffer_t            *buf = ncc_buffer_empty();
    bool                     styled      = false;
    ncc_text_style_t         prev_merged = {0};
    bool                     prev_active = false;

    for (size_t p = 0; p < s->u8_bytes; p++) {
        ncc_text_style_t merged = {0};

        if (si->base_style) {
            merged = *si->base_style;
        }

        ncc_text_case_t active_case = NCC_TEXT_CASE_NONE;
        bool            any_active  = si->base_style != nullptr;

        for (int64_t r = 0; r < si->num_styles; r++) {
            ncc_style_record_t *rec = &si->styles[r];

            if (!rec->info)                                  continue;
            if (rec->start > p)                              continue;
            if (rec->end.has_value && rec->end.value <= p)   continue;

            any_active = true;
            ncc_text_style_t *st = rec->info;

            if (st->bold != NCC_TRI_UNSPECIFIED)             merged.bold = st->bold;
            if (st->dim != NCC_TRI_UNSPECIFIED)              merged.dim = st->dim;
            if (st->italic != NCC_TRI_UNSPECIFIED)           merged.italic = st->italic;
            if (st->underline != NCC_TRI_UNSPECIFIED)        merged.underline = st->underline;
            if (st->double_underline != NCC_TRI_UNSPECIFIED) merged.double_underline = st->double_underline;
            if (st->strikethrough != NCC_TRI_UNSPECIFIED)    merged.strikethrough = st->strikethrough;
            if (st->reverse != NCC_TRI_UNSPECIFIED)          merged.reverse = st->reverse;
            if (st->blink != NCC_TRI_UNSPECIFIED)            merged.blink = st->blink;
            if (ncc_color_is_set(st->fg_rgb))                merged.fg_rgb = st->fg_rgb;
            if (ncc_color_is_set(st->bg_rgb))                merged.bg_rgb = st->bg_rgb;
            if (st->text_case != NCC_TEXT_CASE_NONE)         active_case = st->text_case;
        }

        bool style_changed = (any_active != prev_active)
                          || (any_active && memcmp(&merged, &prev_merged, sizeof(merged)) != 0);

        if (style_changed) {
            if (any_active) {
                emit_sgr(buf, &merged);
                styled = true;
            }
            else if (styled) {
                ncc_buffer_puts(buf, "\x1b[0m");
                styled = false;
            }
        }

        prev_merged = merged;
        prev_active = any_active;

        unsigned char ch = (unsigned char)s->data[p];

        switch (active_case) {
        case NCC_TEXT_CASE_UPPER:
        case NCC_TEXT_CASE_CAPS:
            ncc_buffer_putc(buf, (char)toupper(ch));
            break;
        case NCC_TEXT_CASE_LOWER:
            ncc_buffer_putc(buf, (char)tolower(ch));
            break;
        case NCC_TEXT_CASE_TITLE:
            if (p == 0 || isspace((unsigned char)s->data[p - 1])) {
                ncc_buffer_putc(buf, (char)toupper(ch));
            }
            else {
                ncc_buffer_putc(buf, (char)ch);
            }
            break;
        default:
            ncc_buffer_putc(buf, (char)ch);
            break;
        }
    }

    if (styled) {
        ncc_buffer_puts(buf, "\x1b[0m");
    }

    return ncc_buffer_take(buf);
}
