// WP-002 Phase 1: regression test for the stack-maps default-on
// invariant (D-031). When ncc is invoked WITHOUT any
// --ncc-gc-stack-maps flag and the gc_stack_maps meson option's
// default is `true`, the transform must still fire on a function
// with an eligible stack-local pointer slot. This is the symmetric
// counterpart to test_gc_stack_maps_flag.c (which the
// ncc_gc_stack_maps_default_off test uses with an explicit
// --ncc-no-gc-stack-maps opt-out under the new default).
int
gc_stack_maps_default_on_fixture(int *input)
{
    int *local = input;
    return local ? *local : 0;
}
