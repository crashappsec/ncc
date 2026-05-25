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

// WP-011 Phase 3c.ii.b: variant that also fills the descriptor's
// `cached_hash` slot.  `cached_hash_expr` is a C expression string
// (e.g., a compile-time integer literal) that the rstr template will
// cast to `n00b_uint128_t` for the `.cached_hash=$N` slot.  Pass
// `nullptr` (or `"0"`) for non-key emission; pass a precomputed
// `n00b_uint128_t`-shaped literal for r-string-key emission so the
// runtime `n00b_hash()` short-circuit (D-066) lands the right value
// when a dict lookup walks the static range.  String ownership: the
// caller owns the buffer pointed at by `cached_hash_expr`.
ncc_rstr_static_ref_t ncc_rstr_build_static_ref_ex(
    ncc_xform_ctx_t *ctx,
    ncc_parse_tree_t *node,
    const char *cached_hash_expr);
