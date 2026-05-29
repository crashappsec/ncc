// test_auto_gc_roots_struct_with_pointers.c
//
// Phase 4 regression test (spec § 9 "_struct_with_pointers"). With
// `--ncc-auto-gc-roots` on, a TU-scope struct with mixed pointer /
// non-pointer fields must produce one table entry PER POINTER-RUN of
// pointer-bearing fields (spec § 4.1).
//
// Layout of `mixed`:
//   pointer-bearing word at field `a`         → run #1 (1 word)
//   non-pointer word at field `b` (int)       → breaks run
//   pointer-bearing word at field `c`         → run #2 (1 word)
//
// Expected emit: two `__builtin_offsetof(typeof(mixed), a)` and
// `__builtin_offsetof(typeof(mixed), c)` entries. Meson wiring
// asserts both via `preprocess_contains`.

typedef struct n00b_string_t n00b_string_t;

static struct {
    n00b_string_t *a;
    int            b;
    n00b_string_t *c;
} mixed;

int
main(void)
{
    return (mixed.a == ((void *)0) && mixed.c == ((void *)0) && mixed.b == 0)
               ? 0
               : 1;
}
