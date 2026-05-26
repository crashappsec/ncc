typedef int jmp_buf[1];

void longjmp(jmp_buf env, int value);

typedef struct n00b_gc_stack_frame_t {
    struct n00b_gc_stack_frame_t *prev;
} n00b_gc_stack_frame_t;

typedef struct {
    jmp_buf                n00b_jmp_env;
    n00b_gc_stack_frame_t *n00b_gc_stack_top;
} n00b_jmp_buf_t;

void n00b_gc_stack_restore(n00b_gc_stack_frame_t *top);

void
n00b_longjmp(n00b_jmp_buf_t *ctx, int value)
{
    void *local = ctx;

    n00b_gc_stack_restore(ctx->n00b_gc_stack_top);
    if (local) {
        longjmp(ctx->n00b_jmp_env, value);
    }
}
