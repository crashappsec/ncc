#pragma once

/**
 * @file xform_gc_globals.h
 * @brief Transform: auto-register TU-scope pointer-bearing globals as
 *        libn00b GC roots.
 *
 * When the `--ncc-auto-gc-roots` CLI flag is set (and the matching
 * meson option `auto_gc_roots` was enabled at ncc build time), this
 * pass walks every TU-scope `external_declaration` and — in later
 * phases — emits a per-TU `n00b_gc_root_t` table plus a
 * `[[gnu::constructor]]` registrar that calls
 * `n00b_gc_register_roots(table, count)` (see spec § 4.1, D-005).
 *
 * Phase 1 lands only the no-op skeleton: the pass registers with the
 * transform walk but emits nothing and produces no diagnostics in
 * both flag-off and flag-on states. The eligibility logic, table
 * emission, attribute consumption, and warn-and-skip diagnostics
 * arrive in Phases 2-5.
 *
 * Must run LAST in the xform registration sequence — after rpc,
 * once, kargs_vargs, and static_image — so it sees the final
 * flattened TU including any TU-scope synthetic decls those passes
 * produce (spec § 8.3).
 */

#include "xform/transform.h"

extern void ncc_register_gc_globals_xform(ncc_xform_registry_t *reg);
