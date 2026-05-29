// test_auto_gc_roots_nested_struct.c
//
// Phase 4 regression test (spec § 9 "_nested_struct"). A pointer
// field inside an embedded struct must produce a table entry whose
// address points at the inner field. ncc emits the address via
// `&(name).field.path` (the unary `&` of a designated subobject)
// rather than the `__builtin_offsetof` form — see D-017 / F-1 in
// the DECISIONS log: ncc's grammar restricts `__builtin_offsetof`'s
// second argument to a single identifier, blocking nested
// designators like `in.p`. The `&(outer).in.p` form is
// byte-address-equivalent.
//
// Expected emit (one entry, 1 word):
//     { .addr = (void *) & (outer).in.p, .num_words = 1 },

typedef struct n00b_string_t n00b_string_t;

struct inner {
    n00b_string_t *p;
};

static struct outer {
    int          x;
    struct inner in;
} outer;

int
main(void)
{
    return (outer.x == 0 && outer.in.p == ((void *)0)) ? 0 : 1;
}
