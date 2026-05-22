int
gc_stack_maps_flag_fixture(int *input)
{
    int *local = input;
    return local ? *local : 0;
}
