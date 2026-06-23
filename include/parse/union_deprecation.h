#pragma once

/**
 * @file union_deprecation.h
 * @brief Deprecation diagnostic steering bare C unions toward n00b_variant_t.
 *
 * ncc targets n00b as its runtime base, where the GC-precise, marshalable way
 * to express a tagged union is `n00b_variant_t`. A traditional C union has no
 * discriminator the collector can read, so it forces a conservative scan (and
 * is not precisely marshalable) — exactly the footgun n00b_variant_t avoids.
 *
 * This read-only pass warns on every union that is NOT:
 *   1. an n00b_variant_t value-union (all members named `field_<...>`),
 *   2. in a system header (libc / SDK unions are not ours to change), or
 *   3. annotated `union [[n00b::raw_union]] { ... }` (the escape hatch for
 *      type-punning / FFI-layout / non-n00b-target unions).
 *
 * With `error_on_union` the diagnostic is emitted at error severity and the
 * caller fails the compile. The wording says raw unions *may* become an error
 * in a future release.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"

/**
 * @brief Diagnose traditional (non-variant) unions. Returns the number of
 *        diagnostics emitted. No-op (returns 0) when @p allow_unions is true.
 *
 * Must run PRE-transform (before _generic_struct lowering), so n00b_variant_t
 * value-unions are still in their `_generic_struct typeid("n00b_variant", ...)`
 * source form and excluded by that marker — rather than after lowering, which
 * yields synthesized nodes no type-model query can resolve.
 */
int ncc_union_deprecation_check(ncc_parse_tree_t *tu,
                                bool              allow_unions,
                                bool              error_on_union);
