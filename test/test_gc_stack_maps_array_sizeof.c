enum {
    GC_STACK_MAPS_COUNT = 3,
};

struct gc_stack_maps_array_sizeof_pair {
    void *ptr;
    int   tag;
};

void
gc_stack_maps_array_sizeof_fixture(void *input)
{
    void *vecs[GC_STACK_MAPS_COUNT] = {input, input, input};
    struct gc_stack_maps_array_sizeof_pair combos[] = {
        {input, 1},
        {input, 2},
    };

    (void)vecs;
    (void)combos;
}
