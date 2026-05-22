void *sink;

void
n00b_thread_init(void *arg)
{
    void *local = arg;
    sink = local;
}

void
n00b_thread_destroy(void *arg)
{
    void *local = arg;
    sink = local;
}

void
n00b_capture_stack_top(void *arg)
{
    void *local = arg;
    sink = local;
}

void
_n00b_thread_suspend(void *arg)
{
    void *local = arg;
    sink = local;
}

void
_n00b_stop_the_world(void *arg)
{
    void *local = arg;
    sink = local;
}

void
n00b_wait_for_stw_release(void *arg)
{
    void *local = arg;
    sink = local;
}

void
n00b_futex_wait_timespec(void *arg)
{
    void *local = arg;
    sink = local;
}
