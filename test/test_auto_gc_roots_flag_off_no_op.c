// test_auto_gc_roots_flag_off_no_op.c
//
// Regression test for the no-op-when-off contract of the
// xform_gc_globals transform (spec § 6.1, D-006).
//
// This source would qualify for auto-registration if the flag were
// on — `static_singleton` is a TU-scope pointer-bearing static. The
// test runner invokes ncc *without* `--ncc-auto-gc-roots` and
// asserts (via `preprocess_not_contains` on the post-transform
// emit; ncc's -E flag emits the post-transform C source) that the
// per-TU table marker `__ncc_gc_root_table_` does NOT appear in
// the output. The contract pinned here is that flag-off produces
// zero emission from the auto-roots pass, regardless of what the
// transform's eligibility logic decides in later phases.
//
// The test is wired through `test/run_test.sh` from `meson.build`
// in the `ncc_compile_tests` table (mode `preprocess_not_contains`).
//
// A `main` is provided so the same source remains buildable as a
// plain executable should anyone want to spot-check it; the test
// runner only exercises the `-E` path, but keeping a `main` here
// matches the convention of other test sources in this directory
// (e.g., test_bang.c, test_gc_stack_maps_flag.c).

#include <stdio.h>

typedef struct n00b_string_t n00b_string_t;

// TU-scope pointer scalar that *would* qualify under the
// Phase 2 eligibility rules. With the flag off, no table entry
// should be emitted.
static n00b_string_t *static_singleton;

int
main(void)
{
    // Force the static to be considered "used" so the compile path
    // through `compile_run` (if a future test cares) does not warn.
    printf("static_singleton = %p\n", (void *)static_singleton);
    return 0;
}
