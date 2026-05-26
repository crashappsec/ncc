typedef int jmp_buf[1];

typedef struct {
    jmp_buf n00b_jmp_env;
    void   *n00b_gc_stack_top;
} n00b_jmp_buf_t;

int              setjmp(jmp_buf env);
n00b_jmp_buf_t *n00b_gc_stack_prepare_jmp(n00b_jmp_buf_t *ctx);

int
bad_gc_stack_maps_setjmp_spoof(void *input)
{
    void           *root = input;
    n00b_jmp_buf_t  checkpoint;
    n00b_jmp_buf_t  raw_target;

    if (setjmp((n00b_gc_stack_prepare_jmp(&checkpoint),
                raw_target.n00b_jmp_env))) {
        return root != 0;
    }

    return 0;
}
