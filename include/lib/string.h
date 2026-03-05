#pragma once

#include "ncc.h"
#include "lib/style.h"

struct ncc_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};

#define NCC_STRING_STATIC(lit) \
    ((ncc_string_t){ .data = (char *)(lit), .u8_bytes = sizeof(lit) - 1, \
                      .codepoints = sizeof(lit) - 1, .styling = nullptr })

extern ncc_string_t ncc_string_from_raw(const char *src, int64_t byte_len);
extern ncc_string_t ncc_string_from_cstr(const char *src);
extern ncc_string_t ncc_string_empty(void);
extern bool          ncc_string_eq(ncc_string_t a, ncc_string_t b);
extern char         *ncc_string_to_ansi(const ncc_string_t *s);
