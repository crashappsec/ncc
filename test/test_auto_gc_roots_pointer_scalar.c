// test_auto_gc_roots_pointer_scalar.c
//
// Phase 2 regression test (spec § 9 "_pointer_scalar"). With
// `--ncc-auto-gc-roots` on, two TU-scope pointer-scalar statics
// must be auto-registered as GC roots:
//
//   1. `static n00b_string_t *x = ((void *)0);`
//   2. `static n00b_buffer_t *y = ((void *)0);`
//
// The ncc-emitted post-transform stream must contain:
//   - a single `__ncc_gc_root_table_<TU-uniq>[]` array with one
//     entry per qualifying decl in declaration order,
//   - a single `__ncc_gc_root_section_entry_<TU-uniq>` descriptor
//     in the `n00b_gcroots` linker section.
//
// The meson wiring asserts the presence of the root entries
// for both `x` and `y` via `preprocess_contains`. The runner only
// exercises `ncc -E`, so this source is never compiled to an
// executable; the local typedefs are stubs that let ncc's
// transform see the pointer types it cares about.

typedef struct n00b_string_t n00b_string_t;
typedef struct n00b_buffer_t n00b_buffer_t;

static n00b_string_t *x = ((void *)0);
static n00b_buffer_t *y = ((void *)0);

int
main(void)
{
    return (x == ((void *)0) && y == ((void *)0)) ? 0 : 1;
}
