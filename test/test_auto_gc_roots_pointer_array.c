// test_auto_gc_roots_pointer_array.c
//
// Phase 3 regression test (spec § 9 "_pointer_array"). With
// `--ncc-auto-gc-roots` on, a TU-scope `static n00b_string_t *xs[8];`
// must register as ONE table entry with `num_words = 8` — the entire
// array's address registered as N consecutive pointer-words
// (spec § 4.1).
//
// Meson wiring asserts via `preprocess_contains` that the emitted
// source contains both the `& xs` reference and the `.num_words = 8`
// magnitude. The runner only exercises `ncc -E`, so this source is
// never compiled to an executable; the local typedef is a stub that
// lets ncc's transform see the pointer type it cares about.

typedef struct n00b_string_t n00b_string_t;

static n00b_string_t *xs[8];

int
main(void)
{
    return xs[0] == ((void *)0) ? 0 : 1;
}
