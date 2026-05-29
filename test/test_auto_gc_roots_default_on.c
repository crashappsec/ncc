// WP-003 Phase 1: regression test for the auto-roots default-on
// invariant (D-031). When ncc is invoked WITHOUT any
// --ncc-auto-gc-roots flag and the auto_gc_roots meson option's
// default is `true`, the xform_gc_globals transform must still fire
// on a TU-scope pointer-bearing decl. This is the symmetric
// counterpart to test_auto_gc_roots_flag_off_no_op.c (which
// asserts the no-op contract under the explicit
// --ncc-no-auto-gc-roots opt-out path).
//
// The meson wiring asserts the presence of the per-TU table marker
// `__ncc_gc_root_table_` in the post-transform stream via
// `preprocess_contains` with NO explicit --ncc-auto-gc-roots flag.
//
// Local typedef so ncc's transform sees a pointer-to-aggregate type
// in the eligible spelling without dragging in any libn00b headers
// (this source must work in compile_run paths too, and we only run
// `ncc -E` against it).
typedef struct n00b_string_t n00b_string_t;

static n00b_string_t *auto_gc_roots_default_on_singleton;

int
main(void)
{
    return auto_gc_roots_default_on_singleton == nullptr ? 0 : 1;
}
