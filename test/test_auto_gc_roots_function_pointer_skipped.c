// test_auto_gc_roots_function_pointer_skipped.c
//
// Phase 2 skip-rule regression test (spec § 9
// "_function_pointer_skipped", spec § 2.2 row 2). With
// `--ncc-auto-gc-roots` on, a function-pointer TU-scope definition
// must NOT produce a registration entry: function pointers do not
// point into managed memory.
//
// The meson wiring asserts via `preprocess_not_contains` that the
// emitted source contains no `& fn_ptr` reference. As in the
// `_extern_skipped` test, the source carries a registering decl
// (`registered`) so the auto-roots transform produces a table and
// we are exercising the per-decl skip rule (not the zero-decls
// emit-nothing rule).

typedef struct n00b_string_t n00b_string_t;

static void (*fn_ptr)(void) = ((void *)0);
static n00b_string_t *registered = ((void *)0);

int
main(void)
{
    return (fn_ptr == ((void *)0) && registered == ((void *)0)) ? 0 : 1;
}
