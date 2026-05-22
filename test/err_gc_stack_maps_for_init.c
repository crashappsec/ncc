void
bad_gc_stack_maps_for_init(void *input)
{
    for (void *cursor = input; cursor; cursor = 0) {
    }
}
