typedef int jmp_buf[1];

int setjmp(jmp_buf env);

int
bad_gc_stack_maps_setjmp(void *input)
{
    jmp_buf state = {0};
    void   *local = input;

    if (setjmp(state)) {
        return local != 0;
    }

    return 0;
}
