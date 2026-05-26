# ncc Static Objects Pipeline — User & Contributor Manual

This document covers the static-objects work delivered by WP-003 through
WP-012: every ncc language extension and toolchain feature that
participates in the build-time static-image pipeline.

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
       --ncc-static-init-helper=PATH
```

Not the post-mangle form:

```
// PRE-WP-011 (no longer emitted):
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

## The static-init helper

For every `r"..."`, `b"..."`, `l{...}`, `a{...}`, and nonempty
`d{...}` or `{key:value}` literal, ncc invokes
`n00b-static-init-helper` (a build-time subprocess) to construct the
literal's static-image bytes. The helper:

1. Reads a request from stdin (typed text protocol).
2. Builds the literal's static object in memory using libn00b APIs.
3. Writes the resulting C source (declarations + descriptor + payload)
   to stdout.

ncc then splices the helper's output into the generated C source.

You don't normally interact with the helper directly — ncc's invocation
is automatic when `--ncc-static-init-helper=PATH` is in the c_args
(which `build.sh` adds for all test executables and end-user binaries).

If `--ncc-static-init-helper=PATH` is missing when ncc encounters a
non-empty literal, compilation fails with a clear diagnostic:

```
error: dict literal initializer for 'n00b_dict_t(int, int)' requires
       --ncc-static-init-helper=PATH
```

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

### xform passes touched by WP-003 → WP-012

| Pass | Location | Notes |
|------|----------|-------|
| Rich-string lowering | `src/xform/xform_rstr.c` | r-string template substitution + cached_hash emission (D-077). |
| Buffer literal lowering | `src/xform/xform_array_literal.c` | `b"..."` lowering with cached_hash threading (D-078). |
| Array/list/dict literal lowering | `src/xform/xform_array_literal.c` | The single TU containing array, list, and dict lowering. Big file. |
| Static-image lowering | `src/xform/xform_static_image.c` | Drives the `__ncc_static_image(...)` declaration form. |
| GC stack-map emission | `src/xform/xform_data.h` and others | WP-002 / WP-005 stack maps preserved across non-local exits. |

### Helper protocol (text)

ncc emits requests as plain text to the helper's stdin. The protocol
shape:

```
NCC_STATIC_INIT 1
container_kind <list|array|dict|buffer|rstring>
<key=value lines...>
arg <name> <type> <value>
arg <name> <type> <value>
end
```

Response:
```
NCC_STATIC_INIT_OK <object-expression>
<C source declarations>
```

The helper code lives in
`/Users/viega/n00b/.workspaces/static-generated-objects/src/tools/n00b-static-init-helper.c`.
ncc's `test/static_image_helper.c` is a simplified stub used by ncc's
own tests; Phase 4 of WP-012 added `test_helper_drift` (138th test) to
catch drift between the two.

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

### Phase 5e gap closed

WP-011 Phase 5e applied friendly names to array/list/dict missing-helper
diagnostics. WP-012 Phase 1 audit confirmed no other diagnostic sites
leak the mangled form. The friendly-name treatment can be extended to
future diagnostic categories using the same helper pattern.

### Test fixtures

ncc's `test/` directory contains fixtures for every literal form:

- `test_dict_literal.c` — positive cases (compile and run; lookups
  succeed).
- `err_dict_literal_*.c` — diagnostic cases (compile-time errors with
  specific text).
- `test_list_literal.c`, `test_array_literal.c` — list and array
  precedents.
- `test_helper_drift.c` (WP-012 Phase 4) — drift gate between stub and
  production helpers.

Test stubs vs production helper:

- `test/static_image_helper.c` — ncc's standalone test stub. Simplified
  emission. Doesn't fully mirror production behavior (e.g., it skips
  identity emission, uses a simpler descriptor shape). Phase 5e added
  buffer cached_hash emission; the drift test (WP-012 Phase 4) catches
  future divergence.
- `n00b-static-init-helper` (built from libn00b) — production. Used by
  every test executable and end-user binary.

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

The static-objects pipeline was built across WP-003 → WP-012. Audit
documents and completion notes for each WP live in
`.agents/work-plans/wp-NNN-*/`. Design decisions are recorded as D-001
through D-078 in `.agents/DECISIONS.md`. See `docs/dict_literals.md` in
the n00b workspace for the dict literal user reference and the libn00b
migration recipe.

Known deferrals carried forward past the project end:

- **PE/COFF runtime validation** — needs a Windows runner.
- **D-072: dict migration coordination protocol retirement** —
  concurrency-design WP.
- **D-077/D-078: empty r/b-string cached_hash = 0** — functional, not
  blocking.
- **cstring static descriptors + cached_hash** — future WP. Plain C
  string literals don't currently get descriptors; making them so
  would let `n00b_hash(cstring)` short-circuit.
- **Drift test CI gating** — `test_helper_drift` skips when the
  production helper binary isn't available.
- **Arbitrary constructor-image object literals** — generalize beyond
  list/array/dict/buffer/r-string. Substantial future work.
