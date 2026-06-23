[[n00b::comptime]] int answer = 1;

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    answer = 2;
    return 0;
}

int
main(void)
{
    return answer == 2 ? 0 : 1;
}
