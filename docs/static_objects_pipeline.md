# ncc Static Objects Pipeline — User & Contributor Manual

This document covers the static-objects work delivered by WP-003 through
WP-010: every ncc language extension and toolchain feature
that participates in the build-time static-object pipeline.

Audience split:
- **Part 1 (User Guide)** — for developers writing n00b source that uses
  these features.
- **Part 2 (Contributor Notes)** — for people working on ncc itself.

---

# Part 1 — User Guide

## What ncc does

ncc is a source-to-source C compiler that wraps clang. It adds language
extensions through AST transformations targeting C23. Files without ncc
extensions pass through unchanged. The `--no-ncc` flag bypasses the
entire pipeline for files that need maximum compile speed (e.g., large
generated tables).

The static-objects work this manual describes lets you write
compile-time **literals** for n00b container types (lists, arrays,
dicts), rich strings (`r"..."`), and binary buffers (`b"..."`) — all of
which are emitted as registered static-memory ranges that the n00b
runtime treats as first-class objects.

## Compile-time literal forms

### r-strings — `r"..."`

A rich-text string literal. The content can include markup tags that
ncc parses and lowers into a static `n00b_string_t` with styling
metadata.

```c
n00b_string_t *greeting = r"«b»Hello«/b» world";
n00b_string_t *plain    = r"just text";
n00b_string_t *code     = r"«@code»x = 42«/@code»";
```

Bracket styles `[|i|]italic[|/i|]` and `«b»bold«/b»` both work; the
parser handles either equivalently.

**Static identity.** Every `r"..."` emission produces a registered
static range whose descriptor includes a precomputed content hash in
the `cached_hash` slot (D-077). `n00b_hash(rstring_ptr)` returns the
cached value uniformly, so two content-equal `r"foo"` literals at
different call sites hash to the same value — making them work as dict
keys, set members, etc., without further configuration.

### b-strings — `b"..."`

A binary buffer literal. The content is the raw byte payload of a
`n00b_buffer_t`.

```c
n00b_buffer_t *blob = b"\x00\x01\x02\x03";
n00b_buffer_t *text = b"hello"; // just the 5 bytes h,e,l,l,o
```

**Static identity.** Same as r-strings (D-078). Every `b"..."` emission
populates `cached_hash` with `XXH3_128bits` over the byte payload,
matching `n00b_buffer_hash`. Content-equal buffers hash identically
regardless of where they appear in the source.

### Array literals — `[...]` or `a{...}`

Compile-time arrays of homogeneous elements. The bracketed form is
shorthand for the prefix-modifier form.

```c
n00b_array_t(int) ints  = [1, 2, 3, 4];
n00b_array_t(int) ints2 = a{1, 2, 3, 4};   // equivalent
n00b_array_t(n00b_string_t *) names = [r"alice", r"bob"];
```

**Lifetime rules:** module-scope mutable arrays are allowed;
block-scope mutable arrays are rejected (use a runtime list instead).

Empty `[]` and `a{}` produce empty arrays with null backing storage.

### List literals — `l{...}`

Compile-time `n00b_list_t(T)` literals. Lists are dynamic arrays with
mutability and concurrency semantics distinct from arrays.

```c
n00b_list_t(int) values = l{10, 20, 30};
n00b_list_t(n00b_string_t *) tags = l{r"foo", r"bar"};
```

Static list images are **locked by default** via a runtime rwlock
embedded in the static image (`N00B_STATIC_OBJECT_F_INIT_RWLOCK`).
Capacity rounds up to the next power of two (minimum
`N00B_DEFAULT_LIST_SZ`).

### Dict literals — `d{...}` and bare `{key:value}`

Compile-time `n00b_dict_t(K, V)` literals. See `docs/dict_literals.md`
in the **n00b workspace** for the full reference — that document
covers syntax, supported types, lookup patterns, lock model, the
cached_hash performance path, diagnostics, and the libn00b migration
recipe.

```c
n00b_dict_t(int, int) counts = d{1: 10, 2: 20, 3: 30};
n00b_dict_t(n00b_string_t *, int) name_to_id = d{r"alice": 1, r"bob": 2};
n00b_dict_t(int, int) empty = d{};
```

## Empty `{}` and other compound initializers

ncc preserves all C compound-initializer semantics:

- `{}` — C zero-initialization. Not a dict literal. Empty static dicts
  require `d{}`.
- `{1, 2, 3}` — C compound initializer (struct / array). Not a list.
- `{.x = 5}` — C designated initializer.
- `{key: value, ...}` — Dict literal (the `:` separator triggers it).

## Diagnostic types

Friendly type-name diagnostics use the user-spelled form:

```
error: dict literal initializer for 'n00b_dict_t(int, int)' requires
       a writable file-scope value target
```

Not the post-mangle form:

```
// OLD (no longer emitted):
error: ... initializer for 'struct __JHk7Lxxxx' ...
```

This applies uniformly to array, list, and dict diagnostics.

## Build invocation

### The build.sh wrapper

n00b's `build.sh` is the canonical entry point for building anything
that uses ncc as the compiler. It:

1. Resolves `NCC_PATH` (explicit env, in-tree subproject build, system
   PATH, or cold-start build).
2. Sets `CC=$NCC_PATH` before invoking meson.
3. Passes `-Dusing_build_script=true` so n00b's meson guard accepts the
   invocation.

```sh
bash build.sh build              # default debug build
bash build.sh build_release      # release variant
N00B_TEST=1 bash build.sh build  # build + test
```

### The meson guard (the `~/.local/bin/ncc` staleness trap)

n00b's `meson.build:15` blocks direct `meson setup` invocation:

```meson
if (get_option('using_build_script') != true)
  error('Do not try to run meson directly. Run ./build.sh...')
endif
```

**Why:** if you bypass `build.sh` and pass
`-Dusing_build_script=true` directly to `meson setup`, you lose the
`CC=$NCC_PATH` step. Meson auto-detects whatever clang is on `PATH`,
which then receives `--ncc-*` flags it doesn't understand and fails
the `gen_unicode_lib` / vendor library compile.

If you absolutely must bypass `build.sh`, explicitly pass
`CC=<path-to-ncc>` to `meson setup` alongside
`-Dusing_build_script=true`. Don't rely on `PATH` resolution.

### Per-file `--no-ncc`

Files that should bypass the ncc pipeline (e.g., generated
unicode tables, vendored libraries) can use `--no-ncc` as a c_arg in
their meson target. ncc will pass through unchanged.

## Generalized static initialization

WP-010 retired the bootstrap static-init helper subprocess and the
`--ncc-static-init-helper=PATH` flag. Supported static-object literals
are now lowered in-process by ncc and, for writable file-scope roots,
flow through the generalized comptime image path.

The supported migrated roots include file-scope `b"..."` buffers and
writable value-root array/list/dict literals. Non-migrated literal
shapes now receive targeted diagnostics instead of falling back to a
helper request.

## GC typemaps for typed allocations

In n00b-integrated builds, ncc automatically turns eligible
`typehash(T *)` sites into link-time GC layout metadata. This is what lets
ordinary runtime allocations such as `n00b_alloc(T)` and
`n00b_alloc_array(T, N)` be scanned with the same field precision as
descriptor-backed static objects.

No extra source annotation is needed. The n00b allocation macros already
contain the necessary `typehash(T *)` expression. If `T` is visible at file
scope and ncc can prove every pointer word in the type, ncc emits:

- a `n00b_gc_struct_layout_t` for one element of `T`;
- a pointer-bearing `n00b_gcmap` section entry keyed by `typehash(T *)`;
- a no-pointer `n00b_gcidx` placeholder that the final binary indexes after
  link.

The final executable must be processed by n00b's `n00b-gcmap-index` command.
The n00b build does this for the affected helper binaries and test wrappers.
Manual builds should run:

```sh
path/to/n00b-gcmap-index path/to/executable
```

or execute through:

```sh
path/to/n00b-gcmap-index --exec path/to/executable [args...]
```

If no map entry is available, or if the executable was not indexed, n00b falls
back to conservative default scanning. That fallback is GC-safe but less
precise and may not be enough for marshal workflows that need exact pointer
layout.

For the full ncc-side eligibility rules, see `docs/gc_typemaps.md`. For the
n00b post-link tool, see `docs/gc_type_maps.md` in the n00b repository.

---

# Part 2 — Contributor Notes

## Architecture overview

### Source pipeline

```
Source file (.c)
    │
    ▼
Rich-string scan       (r"«b»text«/b»" → __ncc_rstr("«b»text«/b»"))
    │
    ▼
C preprocessor         (clang -E via pipe)
    │
    ▼
ncc parser             (packrat parser over c_ncc.bnf grammar)
    │
    ▼
xform passes           (AST transformations: array/list/dict literals,
                        static-image lowering, stack maps, GC scan
                        descriptors, identity emission, ...)
    │
    ▼
C emission             (generated C source)
    │
    ▼
clang -c               (object file)
```

### xform passes touched by WP-003 → WP-010

| Pass | Location | Notes |
|------|----------|-------|
| Rich-string lowering | `src/xform/xform_rstr.c` | r-string template substitution + cached_hash emission (D-077). |
| Buffer literal lowering | `src/xform/xform_array_literal.c` | `b"..."` lowering with cached_hash threading (D-078). |
| Array/list/dict literal lowering | `src/xform/xform_array_literal.c` | The single TU containing array, list, and dict lowering. Big file. |
| Buffer static-init lowering | `src/xform/xform_static_image.c` | Lowers file-scope `b"..."` buffer declarations into static-init roots. |
| GC stack-map emission | `src/xform/xform_data.h` and others | WP-002 / WP-005 stack maps preserved across non-local exits. |

### Retired helper protocol

The old helper protocol was removed from the active ncc path in WP-010.
Do not add new code or tests that depend on `test/static_image_helper.c`,
`test_helper_drift`, or `--ncc-static-init-helper=PATH`.

Historical request shape:

```
NCC_STATIC_INIT 1
container_kind <list|array|dict|buffer|rstring>
<key=value lines...>
arg <name> <type> <value>
arg <name> <type> <value>
end
```

Historical response:
```
NCC_STATIC_INIT_OK <object-expression>
<C source declarations>
```

### Cached_hash slot mechanics (D-066, D-077, D-078)

Descriptor-backed static objects can carry a 128-bit cached hash that
short-circuits `n00b_hash()` for pointer-key lookups.

Algorithm match (must be bit-identical with the runtime):

| Object type | ncc compile-time | n00b runtime |
|-------------|------------------|--------------|
| Scalar key bytes | `XXH3_128bits(bytes, sz)` | `n00b_hash_raw` |
| r-string content | `n00b_xxh_convert(XXH3_128bits(s->data, s->u8_bytes))` | `n00b_string_hash` |
| buffer content | `n00b_xxh_convert(XXH3_128bits(b->data, b->byte_len))` | `n00b_buffer_hash` |

ncc emits the hash via `format_rstr_cached_hash_expr` (in `xform_rstr.c`)
and `compute_buffer_key_hash` (in `xform_array_literal.c`). The vendored
`include/vendor/xxhash.h` is byte-identical with n00b's vendor copy.

Empty inputs (`r""`, `b""`) emit `cached_hash = 0` because the runtime
`n00b_hash_word(0ULL)` is a non-zero constant ncc cannot reproduce at
compile time. The recompute-then-vtable path handles this correctly.

### Diagnostic friendly names

`xform_array_literal.c` exposes three helpers:

```c
char *list_type_friendly_name(const list_type_info_t *type);
char *array_type_friendly_name(const array_type_info_t *type);
char *dict_type_friendly_name(const dict_type_info_t *type);
```

They synthesize the user-spelled form (`n00b_list_t(int)`,
`n00b_dict_t(K, V)`, etc.) from the per-type element metadata. The
post-mangle `object_type` field is reserved for helper-protocol emission
where the type hash matters; user-visible diagnostics use the friendly
form.

### Diagnostic guidance

Friendly names should be used for user-facing array/list/dict diagnostics
rather than post-mangle type names. The same treatment can be extended to
future diagnostic categories using the local helper pattern.

### Test fixtures

ncc's `test/` directory contains fixtures for migrated literal forms:

- `err_dict_literal_*.c` — diagnostic cases (compile-time errors with
  specific text).
- `test_array_literal.c` — array literal precedents that lower without
  the helper.
- `test_*_static_migrated.c` and `test_*_baking_no_helper.c` —
  comptime-image E2E coverage for migrated buffer/list/array/dict roots.

### Vendored xxhash

`include/vendor/xxhash.h` is the upstream xxHash header, BSD 2-Clause
licensed. ncc and n00b both link against it. Bit-identity between the
two trees is required for static-key lookups to work.

## Build flake to know about

`test_cocoa_backend` may fail to link on macOS for environmental reasons
(missing system framework dep, etc.). It's not part of the static-objects
pipeline. When running focused tests after a partial rebuild, use
`--no-rebuild` to side-step.

## Project workplans (handoff context)

The static-objects pipeline was built across WP-003 → WP-010. Audit
documents and completion notes for each WP live in
`.agents/work-plans/wp-NNN-*/`. Design decisions are recorded in
`.agents/DECISIONS.md`. See the n00b workspace documentation for runtime-side
user references and migration notes.

Known deferrals carried forward past the project end:

- **PE/COFF runtime validation** — needs a Windows runner.
- **D-072: dict migration coordination protocol retirement** —
  concurrency-design WP.
- **D-077/D-078: empty r/b-string cached_hash = 0** — functional, not
  blocking.
- **cstring static descriptors + cached_hash** — future WP. Plain C
  string literals don't currently get descriptors; making them so
  would let `n00b_hash(cstring)` short-circuit.
- **Arbitrary constructor-image object literals** — generalize beyond
  list/array/dict/buffer/r-string. Substantial future work.
