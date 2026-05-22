void
bad_gc_stack_maps_vla(int n, void *input)
{
    void *items[n];
    items[0] = input;
}
