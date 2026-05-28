// test_auto_gc_roots_extern_skipped.c
//
// Phase 2 skip-rule regression test (spec § 9 "_extern_skipped",
// spec § 2.2 row 1). With `--ncc-auto-gc-roots` on, an `extern
// T *x;` declaration must NOT produce a registration entry: the
// definition's TU is responsible.
//
// The meson wiring asserts via `preprocess_not_contains` that the
// emitted source contains no `& extern_singleton` reference (the
// shape ncc emits for a registered entry: `(void *) & <name>`).
// The flag is on, so the test source also includes a *registering*
// decl so the file is not skipped entirely by the zero-qualifying-
// decls rule (spec § 4.3, which would emit nothing and trivially
// satisfy "no `& extern_singleton`"). The presence of `local_root`
// means a table is produced; the absence of the extern entry is
// what we are pinning.

typedef struct n00b_string_t n00b_string_t;

extern n00b_string_t *extern_singleton;
static n00b_string_t *local_root = ((void *)0);

int
main(void)
{
    return (local_root == ((void *)0) && extern_singleton == ((void *)0)) ? 0 : 1;
}
