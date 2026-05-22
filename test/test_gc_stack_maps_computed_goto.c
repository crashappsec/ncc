int
gc_stack_maps_computed_goto_fixture(void *input, int selector)
{
    void *local = input;
    void *targets[] = {&&done, &&other};

    if (selector < 0 || selector > 1) {
        return 0;
    }

    goto *targets[selector];

done:
    return local != 0;

other:
    return local == 0;
}

