[[n00b::nogc]] int
gc_stack_maps_nogc_fixture(int *input)
{
    int *local = input;
    return local ? *local : 0;
}
