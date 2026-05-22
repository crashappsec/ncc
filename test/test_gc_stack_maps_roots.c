struct gc_stack_maps_box {
    void *ptr;
    int   tag;
};

void
gc_stack_maps_roots_fixture(void *param,
                            int *items,
                            struct gc_stack_maps_box box_param)
{
    void *local = param;
    void *slots[3] = {param, items, local};
    struct gc_stack_maps_box box = {.ptr = local, .tag = 1};

    {
        int *nested = items;
        (void)nested;
    }

    (void)slots;
    (void)box;
    (void)box_param;
}
