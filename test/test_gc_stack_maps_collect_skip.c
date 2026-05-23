typedef int jmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value);

void
n00b_collect(void *arena)
{
    jmp_buf state = {0};
    void   *local = arena;

    if (!setjmp(state)) {
        (void)local;
        longjmp(state, 1);
    }
}
