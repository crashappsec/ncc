// test_auto_gc_roots_pointer_run_coalescing.c
//
// Phase 4 regression test (spec § 4.1 — pointer-run coalescing).
// Two adjacent pointer fields must coalesce into ONE table entry
// with `num_words = 2`, not two entries with `num_words = 1`.
//
// Layout of `pair`:
//   pointer-bearing word at field `a`  ┐
//   pointer-bearing word at field `b`  ┘ coalesce: one run, 2 words
//   non-pointer word at field `c`        breaks the run
//
// Expected emit (one entry; ncc emits the run's start address as
// `&(name).<first-field>` per D-017 / F-1 — the `__builtin_offsetof`
// form is blocked by an ncc grammar restriction on nested designators):
//     { .addr = (void *) & (pair).a, .num_words = 2 },

typedef struct n00b_string_t n00b_string_t;
typedef struct n00b_buffer_t n00b_buffer_t;

static struct {
    n00b_string_t *a;
    n00b_buffer_t *b;
    int            c;
} pair;

int
main(void)
{
    return (pair.a == ((void *)0) && pair.b == ((void *)0) && pair.c == 0)
               ? 0
               : 1;
}
