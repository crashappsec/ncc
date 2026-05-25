#pragma once

#include <stddef.h>
#include <stdint.h>

#include "xform/transform.h"

typedef struct {
    char    *decl;
    char    *expr;
    // WP-011 Phase 3c.ii.a: post-rich-markup UTF-8 content of the
    // r-string.  Populated alongside the decl/expr so callers in
    // r-string-as-dict-key contexts can feed the same byte sequence
    // into `compute_string_key_hash` (which mirrors
    // `n00b_string_hash`'s XXH3_128bits over `s->data, s->u8_bytes`).
    // Standalone r-string emission paths ignore these fields; the
    // pointer is freshly allocated and owned by the caller (free with
    // `ncc_free`).  `content` is NULL and `content_len` is 0 only on
    // error paths where decl/expr are also NULL.
    char    *content;
    size_t   content_len;
} ncc_rstr_static_ref_t;

ncc_rstr_static_ref_t ncc_rstr_build_static_ref(ncc_xform_ctx_t *ctx,
                                                ncc_parse_tree_t *node);
