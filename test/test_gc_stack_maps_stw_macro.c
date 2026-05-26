typedef int jmp_buf[1];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value);

void  n00b_capture_stack_top(void *thread);
void *_ncc_stw_thread_self(void);
void  _n00b_thread_suspend(char *loc);
void  _n00b_thread_resume(char *loc);
void  n00b_thread_checkin(void);

#line 1 "include/core/stw.h"
#define N00B_LOC_STRING() "test"
#define n00b_thread_self() _ncc_stw_thread_self()
#define n00b_run_blocking(...)                                                                 \
    {                                                                                          \
        jmp_buf save_state = {0};                                                              \
        if (!setjmp(save_state)) {                                                             \
            n00b_capture_stack_top(n00b_thread_self());                                        \
            _n00b_thread_suspend(N00B_LOC_STRING());                                           \
            __VA_ARGS__;                                                                       \
            _n00b_thread_resume(N00B_LOC_STRING());                                            \
            longjmp(save_state, 1);                                                            \
        }                                                                                      \
        else {                                                                                 \
            n00b_thread_checkin();                                                             \
        }                                                                                      \
    }
#line 100 "test/test_gc_stack_maps_stw_macro.c"

int
gc_stack_maps_stw_macro_fixture(void *input)
{
    void *local = input;
    int   n     = 0;

    n00b_run_blocking(n = 1);

    return local != 0 && n;
}
