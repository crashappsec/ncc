# GC Typemaps for n00b Allocations

This page documents the ncc side of n00b heap allocation type maps. It is for
people compiling n00b-integrated C code with ncc, and for embedding runtimes
that provide the same GC metadata ABI.

## What ncc Emits

When ncc transforms a translation unit, every `typehash(T *)` expression is
also treated as a possible request for a GC layout record for `T`. If ncc can
prove the layout of `T` precisely enough, it appends file-scope C declarations
for:

- a `n00b_gc_struct_layout_t` describing the pointer-word offsets in one `T`;
- a `n00b_gc_type_map_entry_t` in the `n00b_gcmap` linker section, keyed by
  the exact `typehash(T *)` value;
- a matching `n00b_gc_type_map_index_entry_t` placeholder in the `n00b_gcidx`
  linker section.

The `n00b_gcmap` entry contains a relocated pointer to the layout descriptor.
The `n00b_gcidx` entry intentionally contains no pointers. n00b's post-link
indexing tool fills and sorts only `n00b_gcidx`, so it does not disturb linker
fixups in the pointer-bearing `n00b_gcmap` section.

For normal n00b code there is no separate source annotation or ncc flag. The
allocation macros already pass `typehash(T *)` to the runtime:

```c
typedef struct widget_t {
    n00b_string_t *name;
    uint64_t       count;
} widget_t;

widget_t *one = n00b_alloc(widget_t);
widget_t *many = n00b_alloc_array(widget_t, 16);
```

Those macro expansions create the `typehash(widget_t *)` sites that let ncc
emit the matching type map entry.

## Runtime Effect

The linked binary must be indexed with `n00b-gcmap-index` before execution.
At runtime, `_n00b_alloc_raw()` uses the allocation's `typehash(T *)` to look
up a layout. If a valid indexed entry exists and the caller did not specify a
custom scan policy, the allocation is upgraded from conservative `DEFAULT`
scanning to `CALLBACK` scanning with `n00b_gc_scan_cb_type_layout`.

This has two visible effects:

- The GC scans only the pointer fields ncc proved are real heap edges.
- Marshal can round-trip these heap objects using an explicit callback-scan
  record instead of rejecting or over-scanning the allocation shape.

If no entry exists, or if the final executable was not indexed, n00b falls back
to the old conservative `DEFAULT` behavior. That fallback is GC-safe, but it is
less precise and may be insufficient for marshal workflows that require exact
pointer layout.

## Eligible Types

ncc is intentionally conservative. It emits a type map only when it can describe
every GC pointer word without guessing.

Eligible `typehash()` shapes:

- `typehash(T *)` for one pointer level to `T`;
- file-scope-visible aggregate definitions;
- scalar or no-pointer types, which produce a zero-pointer layout;
- structs with direct data-pointer fields;
- structs with nested by-value structs whose layouts are fully visible;
- structs with function-pointer fields, which are skipped because code
  pointers are not GC heap edges;
- unions whose active storage is always pointer-shaped or always scalar.

Skipped shapes:

- `void *`, `T **`, pointer arrays, and top-level atomic pointer hashes;
- block-local typedefs or tags that would be out of scope for appended
  file-scope declarations;
- unresolved or incomplete aggregate types;
- data-pointer arrays and aggregate arrays;
- unions that mix pointer and non-pointer alternatives;
- aggregate layouts requiring more than the internal safety cap for emitted
  pointer offsets.

ncc also recognizes a n00b runtime-scan struct shape containing
`n00b_gc_scan_kind_t scan_kind`, `n00b_gc_scan_cb_t scan_cb`, and pointer
`scan_user`. For that exact shape, runtime infrastructure fields such as
`scan_cb`, `scan_user`, `lock`, and `allocator` are skipped while ordinary
payload pointers are still mapped.

## Build Requirements

The generated declarations reference the n00b GC metadata ABI:

- `n00b_gc_struct_layout_t`
- `n00b_gc_type_map_entry_t`
- `n00b_gc_type_map_index_entry_t`
- the platform section attributes for `n00b_gcmap` and `n00b_gcidx`

n00b builds provide these through the normal n00b headers. If another runtime
embeds ncc and wants this feature, it must provide compatible declarations and
must run an equivalent post-link index pass.

The n00b build script wires this up for n00b targets. For manual builds, invoke
the built `n00b-gcmap-index` tool after the final link:

```sh
ncc -c module.c
cc module.o -o app
path/to/n00b-gcmap-index app
./app
```

For test runners or one-shot execution,
`path/to/n00b-gcmap-index --exec app [args...]` indexes in place and then
`execv()`s the program.

## Inspecting Output

Use ncc's transformed-source output when debugging an emitted map:

```sh
ncc -E module.c
```

Look near the end of the emitted C for declarations named
`__ncc_gcmap_lay_*`, `__ncc_gcmap_ent_*`, and `__ncc_gcidx_ent_*`.
Missing declarations mean ncc skipped the type because the shape was not a
safe candidate.

For the n00b-side post-link tool and runtime lookup behavior, see the n00b
repository's `docs/gc_type_maps.md`.
