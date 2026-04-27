int
main(void)
{
    return constexpr_eval(1 / *(volatile int *)&(int){0});
}
