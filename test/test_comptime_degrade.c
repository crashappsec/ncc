#include <stdio.h>

[[n00b::comptime]] int answer = 1;

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    answer = 77;
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }

    FILE *f = fopen(argv[1], "a");
    if (!f) {
        return 3;
    }
    fprintf(f, "answer=%d\n", answer);
    fclose(f);

    return answer == 77 ? 0 : 4;
}
