typedef int jmp_buf[1];

int setjmp(jmp_buf env);

typedef struct n00b_gc_stack_frame_t {
    struct n00b_gc_stack_frame_t *prev;
} n00b_gc_stack_frame_t;

typedef struct {
    jmp_buf                n00b_jmp_env;
    n00b_gc_stack_frame_t *n00b_gc_stack_top;
} n00b_jmp_buf_t;

n00b_jmp_buf_t *n00b_gc_stack_prepare_jmp(n00b_jmp_buf_t *ctx);
void            n00b_longjmp(n00b_jmp_buf_t *ctx, int value);

#define n00b_setjmp(ctx) setjmp(n00b_gc_stack_prepare_jmp((ctx))->n00b_jmp_env)

n00b_jmp_buf_t *
select_checkpoint(n00b_jmp_buf_t *primary, n00b_jmp_buf_t *fallback)
{
    return primary ? primary : fallback;
}

int
gc_stack_maps_nonlocal_api_fixture(n00b_jmp_buf_t *ctx, void *input, int jump)
{
    void *local = input;

    if (!n00b_setjmp(ctx)) {
        if (jump) {
            n00b_longjmp(ctx, 7);
        }
        return local != 0;
    }

    return local != 0;
}

int
gc_stack_maps_nonlocal_api_call_arg(n00b_jmp_buf_t *primary,
                                    n00b_jmp_buf_t *fallback,
                                    void           *input)
{
    void *local = input;

    if (!n00b_setjmp(select_checkpoint(primary, fallback))) {
        return local != 0;
    }

    return local != 0;
}
