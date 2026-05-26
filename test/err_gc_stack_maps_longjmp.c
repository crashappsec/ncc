typedef int jmp_buf[1];

void longjmp(jmp_buf env, int value);

void
bad_gc_stack_maps_longjmp(jmp_buf state, void *input)
{
    void *local = input;

    if (local) {
        longjmp(state, 1);
    }
}
