#pragma once

#include <stddef.h>
#include <stdbool.h>
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

// WP-011 Phase 3c.ii.b / Phase 5d: variant that fills the descriptor's
// `cached_hash` slot.  `cached_hash_expr` is a C expression string
// (e.g., a compile-time integer literal) that the rstr template will
// cast to `n00b_uint128_t` for the `.cached_hash=$N` slot.
//
// Phase 5d changed the default-path contract: pass `nullptr` (or `""`)
// and the implementation computes XXH3_128bits over the post-rich-
// markup UTF-8 content and emits an equivalent `_BitInt(128)`
// expression itself.  Pass a precomputed value to override (e.g., from
// the dict-key path, which threads the same XXH3 through both the
// bucket `hv` slot and this expression for bit-identity).  String
// ownership: the caller owns the buffer pointed at by
// `cached_hash_expr`.
ncc_rstr_static_ref_t ncc_rstr_build_static_ref_ex(
    ncc_xform_ctx_t *ctx,
    ncc_parse_tree_t *node,
    const char *cached_hash_expr);

typedef struct {
    char *expr;
    bool  has_style;
} ncc_rstr_managed_expr_t;

ncc_rstr_managed_expr_t ncc_rstr_build_plain_managed_expr(
    ncc_parse_tree_t *node);
