int
comptime_main(int argc, char **argv, char **envp)
{
    return argc == 0 && argv == 0 && envp == 0;
}

int
main(void)
{
    return 0;
}
