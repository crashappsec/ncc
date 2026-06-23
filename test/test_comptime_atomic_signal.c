[[n00b::comptime]] int answer = 1;

static void
crash_for_comptime(void)
{
#ifdef _WIN32
    volatile int *p = (volatile int *)0;
    *p = 1;
#else
    extern void abort(void);
    abort();
#endif
}

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    crash_for_comptime();
    return 0;
}

int
main(void)
{
    return answer;
}
