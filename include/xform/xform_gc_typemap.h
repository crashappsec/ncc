#pragma once

// WP-020 / D-049: emit per-type GC pointer-map descriptors for the link-time
// type->GC-map dictionary (libn00b's pointer-bearing `n00b_gcmap` section plus
// the post-link-filled no-pointer `n00b_gcidx` search index). The hash key is
// recorded from `typehash(AGG *)` sites so it matches the alloc-site
// `typehash(T*)` byte-for-byte (same computation), and the pointer-offset
// layout is emitted with `__builtin_offsetof` (the C compiler does the
// arithmetic; we only emit field paths for pointer members).

#include "xform/transform.h"

#include <stdint.h>

// Record a typehash occurrence (called from xform_typehash). `type_str` is the
// normalized full type string the hash was computed from (e.g.
// "n00b_nonterm_t*"); `hash` is that typehash. A no-op unless `type_str` names
// a single pointer to an aggregate type whose full definition is known in this
// TU. Idempotent per hash.
void ncc_gc_typemap_note(ncc_xform_ctx_t *ctx,
                         const char     *type_str,
                         uint64_t        hash);

// Emit the C text (descriptors + n00b_gcmap section entries) for everything
// recorded this TU. Returns a heap string (caller frees) — empty if nothing
// qualifies. Drains the accumulator. Call after ncc_xform_apply, before the
// transform dicts are freed.
char *ncc_gc_typemap_emit(ncc_xform_ctx_t *ctx);
