# Precise struct-arm variant typemaps + union deprecation

Status: planned (tracking doc)
Scope: ncc emitter + the shared n00b GC-metadata ABI + n00b consumers.
See also: `docs/gc_typemaps.md` (the existing link-time type→GC-map machinery).

ncc targets n00b as its expected runtime base (you *can* build against another
runtime, but n00b is the default), so both changes below are reasonable
ncc-level defaults.

---

## Change 1 — precise GC typemaps for struct-arm `n00b_variant_t`

### Problem
`n00b_variant_t(...)` lowers to `{ uint64_t selector; union { ...field_<typeid>... } value; }`.
Today ncc's variant typemap models `value` as **one word** that is a heap
pointer iff `selector` is one of a set of "pointer alternative" typehashes. So
it only handles arms that are themselves a single pointer (`ALT_PTR`) or a
scalar (`ALT_NONPTR`). A by-value **struct** arm (pointers at several offsets)
is `ALT_UNSUPPORTED` (`src/xform/xform_gc_typemap.c`, `classify_variant_alt`,
the `total == 0` non-scalar branch), which downgrades the whole enclosing type
to conservative `DEFAULT` scan — imprecise (false-retention on scalar fields)
and "not precisely marshalable".

This is what blocks `n00b_draw_cmd_t` (arms are structs carrying `string`/
`style` + coords) and `n00b_json_node_t` and every other struct-arm variant from
precise scanning.

### Fix — per-selector pointer-offset tables
Generalize "one pointer word per selector" → "a set of pointer offsets per
selector". A single-pointer arm is just `ptr_offsets = {0}`, so one
representation subsumes both.

#### Shared ABI (n00b `include/n00b.h`) — lands atomically with the emitter
```c
typedef struct {                 // one alternative (arm)
    uint64_t        selector;        // typehash(T) of this arm
    uint64_t        ptr_offset_count;
    const uint64_t *ptr_offsets;     // word offsets within value, sorted asc
} n00b_gc_variant_arm_t;

typedef struct {
    uint64_t                     selector_offset;  // word offset of selector
    uint64_t                     value_offset;     // word offset of value union
    uint64_t                     arm_count;
    const n00b_gc_variant_arm_t *arms;             // sorted by selector
} n00b_gc_variant_field_t;
```
Offsets in each arm are relative to `value_offset` (every union member starts at
the union base, i.e. value-relative offset 0), so an arm's internal pointer
offsets — computed by the existing struct walker — drop in directly.

#### ncc emitter (`src/xform/xform_gc_typemap.c`)
- `classify_variant_alt`: add `ALT_AGGREGATE`. In the `total == 0` non-scalar
  branch, resolve the arm's aggregate definition by spelling and compute its
  pointer offsets instead of bailing. If the arm is itself not fully
  describable (nested unsupported union/atomic/etc.), keep `ALT_UNSUPPORTED`
  (whole variant → conservative). **Invariant preserved: never under-scan.**
- Reuse `gc_walk` (the tolerant struct pointer-offset walker) factored to return
  an offset list for an arm type with `base=""`.
- `walk_variant`: emit one `static const uint64_t` offset array per
  pointer-bearing arm + an `arms[]` table sorted by selector, replacing the flat
  `ptr_hashes` list.

#### n00b consumers (all read the shared layout)
- `src/core/gc_map.c` (`n00b_gc_map_mark_struct_layout_count` + helper): replace
  the bool `selector ∈ ptr_hashes` test with an arm lookup; for the matched arm,
  mark `base + value_offset + arm->ptr_offsets[k]` for each offset. Extend the
  bounds `n00b_require`s (`value_offset + max_offset < stride`, within
  `num_words`).
- `src/util/marshal.c` (walks `n00b_gc_struct_layout_t` at `:782,1075,2576`):
  follow per-arm offsets too — this is what clears "not precisely marshalable".
- `src/slay/codegen.c`: builds layouts for JIT/runtime-created types; if any
  carry struct-arm variants, emit the new arm shape. Audit; guard if not needed
  yet (see open question).
- `src/typecheck/print.c`: diagnostics dump — cosmetic update for the new shape.

### Safety
- "Never under-scan": any uncertainty about an arm ⇒ whole variant conservative.
- `selector == 0` (unset) ⇒ mark nothing (existing guard).
- Conservative fallback remains GC-safe; this change only adds precision.

### Tests
- ncc `test/test_gc_typemap.c`: a struct-arm variant (mirroring `draw_cmd`) →
  assert no conservative warning + correct `arms[]`; a nested-unsupported arm →
  assert it still falls back conservatively.
- n00b: allocate a struct-arm variant, set each arm, force a compacting
  collection, assert (a) arm pointers are forwarded and (b) a scalar field whose
  value looks like a heap address is NOT followed (proves precision). Marshal
  round-trip of a struct-arm variant.

### Rollout (lockstep, cross-repo)
The descriptor is shared and n00b builds with a pinned ncc, so:
1. ncc PR (ABI struct in n00b first or simultaneously).
2. Bump the ncc the n00b build uses.
3. n00b PR with the format + consumer changes.
Rebuild n00b and diff gc-typemap warnings: confirm `draw_cmd`, `json_node`, and
other struct-arm variants flip conservative → precise, and nothing regresses.

### Effort
~1–2 days. Emitter (arm-type resolution + `gc_walk` reuse) is the load-bearing
piece; the n00b scanner is small; marshal/codegen localized; then tests + the
lockstep bump + a full rebuild.

### Open questions
1. All-scalar aggregate arm: empty offset list, or classify `ALT_NONPTR`
   (no arm entry)? Latter is leaner.
2. `codegen.c` JIT path: any JIT types with struct-arm variants today, or is
   this purely link-time (so codegen is just guarded, not changed now)?

---

## Change 2 — deprecation warning for traditional unions

Steer all bare C unions toward `n00b_variant_t` (GC-precise + marshalable after
Change 1). Wording uses "may", not "will".

### Trigger
Every `union` specifier that is **not**:
1. an `n00b_variant_t` value-union — reuse the `field_<typeid>`-naming detector
   in `xform_gc_typemap.c` (every union member named `field_…`);
2. in a system header — reuse `ncc_layout_node_starts_in_system_header()`
   (`src/xform/xform_type_layout.c`) / `node_in_system_header()`
   (`src/xform/transform.c`); ncc has per-token file/line provenance from the
   preprocessor `#`-line markers, so this is reliable (and gives the warning its
   `file:line` and lets us dedup a widely-included header union to one
   diagnostic);
3. opted out per-union via `[[n00b::raw_union]]` — following the
   `[[n00b::noscan]]` / `[[n00b::nogc]]` precedent
   (`ncc_xform_subtree_carries_n00b_named_attr`), for type-punning / FFI layout /
   non-n00b-target cases.

### Placement
A dedicated traversal over union specifiers (the gc-typemap pass only sees
unions inside scanned heap aggregates; we want all of them). Can ride an
existing full-tree structural walk (`transform.c` already traverses declarations
and has the system-header helper). One warning per offending union declaration,
deduped by source location.

### Wording
```
ncc: warning: traditional C union '<name>' is discouraged; use n00b_variant_t
for a GC-precise, marshalable tagged union. Raw unions may become an error in a
future release (suppress with [[n00b::raw_union]]).
```

### Knobs
- `--ncc-error-on-union` — escalate warning → error now (per-project opt-in
  ahead of the global change).
- `--ncc-allow-unions` — global suppress for the "building against another
  runtime" case. Off by default (n00b is the expected base).

### Effort
Small — a few hours on top of Change 1: one traversal + three exclusion checks,
all reusing existing helpers and the attribute plumbing.

### Open question
Warn on every non-system, non-variant union (incl. pure stack/FFI scratch), or
only GC-reachable ones? Plan: **every** such union, with `[[n00b::raw_union]]`
as the release valve — surfaces more sites than just GC-relevant ones.
