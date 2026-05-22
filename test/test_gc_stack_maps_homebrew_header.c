#line 1 "/opt/homebrew/include/fake_inline.h"
static inline int
ncc_fake_homebrew_inline(void *input)
{
    void *local = input;
    return local != 0;
}

#line 1 "test/test_gc_stack_maps_homebrew_header.c"
int
gc_stack_maps_homebrew_header_fixture(void *input)
{
    void *local = input;
    return ncc_fake_homebrew_inline(local);
}
