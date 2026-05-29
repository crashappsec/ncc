// test_auto_gc_roots_atomic_pointer_scalar.c
//
// WP-003 / D-036 (F-3) regression test. A TU-scope `_Atomic(T *)`
// global is a single opaque pointer-word root, not a struct whose
// fields should be walked. The transform must:
//   * register `_Atomic(T *) p;` as one scalar entry (num_words = 1)
//     and NOT attempt to walk the pointed-to struct's fields;
//   * skip `_Atomic(int) i;` (no GC-relevant pointer).
//
// The meson wiring asserts:
//   * `preprocess_contains` for `(void *) & atomic_ptr`
//   * `preprocess_contains` for `.num_words = 1` (the scalar shape)
//   * `preprocess_not_contains` for `(void *) & atomic_scalar`
//
// This locks in the F-3 disposition from D-036.

typedef struct opaque_with_ptrs {
    void *a;
    void *b;
    void *c;
} opaque_with_ptrs;

// `_Atomic(opaque_with_ptrs *)` — must register as ONE scalar pointer
// entry, NOT three field entries from the inner struct.
_Atomic(opaque_with_ptrs *) atomic_ptr = ((void *)0);

// `_Atomic(int)` — non-pointer payload; must be skipped.
_Atomic(int) atomic_scalar = 0;

// Anchor decl so the table is not skipped entirely by the zero-
// qualifying-decls rule.
static void *plain_ptr = ((void *)0);

int
main(void)
{
    (void)atomic_ptr;
    (void)atomic_scalar;
    return (plain_ptr == ((void *)0)) ? 0 : 1;
}
