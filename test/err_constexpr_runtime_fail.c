int
main(void)
{
#if defined(__aarch64__) || defined(__arm__)
    /* AArch64/ARM define integer division by zero to return 0 silently
       rather than trap, so the helper would exit 0 and the test
       framework's expected-failure path would never fire.  Use an
       explicit trap on these targets instead. */
    return constexpr_eval((__builtin_trap(), 0));
#else
    return constexpr_eval(1 / *(volatile int *)&(int){0});
#endif
}
