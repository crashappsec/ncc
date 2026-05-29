// test_auto_gc_roots_ncc_emitted_skipped.c
//
// Phase 2 skip-rule regression test (spec § 2.2 row 3 — declarator
// inside `__ncc_static_image(...)` form or other ncc-emitted
// static-object range). With `--ncc-auto-gc-roots` on, any
// TU-scope pointer decl whose identifier begins with `__ncc_` must
// NOT produce a registration entry — those names belong to
// ncc-emitted static-object pipelines (static-image, once-guard,
// buflit, future ones) that register their pointer slots via their
// own machinery.
//
// The meson wiring asserts:
//   - `preprocess_not_contains` "& __ncc_marker_static"  (skipped).
//   - `preprocess_contains`     "& regular_static"        (registered).
//
// Both assertions are needed: presence-of-the-regular entry
// confirms the transform is firing on this source; absence-of-the-
// `__ncc_` entry confirms the prefix skip rule.

typedef struct n00b_string_t n00b_string_t;

static n00b_string_t *__ncc_marker_static = ((void *)0);
static n00b_string_t *regular_static      = ((void *)0);

int
main(void)
{
    return (__ncc_marker_static == ((void *)0)
            && regular_static == ((void *)0))
               ? 0
               : 1;
}
