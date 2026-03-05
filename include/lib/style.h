#pragma once

// Style types for ncc rich strings (r"..." transform output).
//
// Shared between ncc_runtime.h (standalone, for ncc-compiled code)
// and core/string.h (libncc internals).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t ncc_color_t;

typedef enum {
    NCC_TRI_UNSPECIFIED = 0,
    NCC_TRI_NO,
    NCC_TRI_YES,
} ncc_tristate_t;

typedef enum {
    NCC_TEXT_CASE_NONE = 0,
    NCC_TEXT_CASE_UPPER,
    NCC_TEXT_CASE_LOWER,
    NCC_TEXT_CASE_TITLE,
    NCC_TEXT_CASE_CAPS,
} ncc_text_case_t;

typedef struct ncc_text_style_t {
    ncc_tristate_t  bold;
    ncc_tristate_t  italic;
    ncc_tristate_t  underline;
    ncc_tristate_t  double_underline;
    ncc_tristate_t  strikethrough;
    ncc_tristate_t  reverse;
    ncc_tristate_t  dim;
    ncc_tristate_t  blink;
    ncc_text_case_t text_case;
    ncc_color_t     fg_rgb;
    ncc_color_t     bg_rgb;
} ncc_text_style_t;

typedef struct ncc_option_size {
    bool   has_value;
    size_t value;
} ncc_option_size_t;

typedef struct ncc_style_record_t {
    ncc_text_style_t *info;
    const char       *tag;
    size_t            start;
    ncc_option_size_t end;
} ncc_style_record_t;

typedef struct ncc_string_style_info_t {
    int64_t            num_styles;
    ncc_text_style_t  *base_style;
    ncc_style_record_t styles[];
} ncc_string_style_info_t;

#define NCC_COLOR_VALID_BIT ((ncc_color_t)(1 << 31))
#define ncc_color_is_set(c) (((c) & NCC_COLOR_VALID_BIT) != 0)
#define ncc_color_rgb(c)    ((c) & 0x00FFFFFF)
