// test_auto_gc_roots_function_pointer_array_skipped.c
//
// Phase 3 skip-rule regression test. With `--ncc-auto-gc-roots` on,
// a TU-scope array of function pointers
// (`static void (*fns[8])(void);`) must NOT produce a registration
// entry: function pointers do not point into managed memory
// (spec § 2.2 row 2 extended to arrays per Phase 3).
//
// The source carries a registering pointer-array decl alongside so
// the auto-roots transform produces a table and we exercise the
// per-decl skip rule (not the zero-decls emit-nothing rule of
// spec § 4.3).

typedef struct n00b_string_t n00b_string_t;

static void (*fns[8])(void);
static n00b_string_t *registered[4];

int
main(void)
{
    return (fns[0] == ((void *)0) && registered[0] == ((void *)0)) ? 0 : 1;
}
