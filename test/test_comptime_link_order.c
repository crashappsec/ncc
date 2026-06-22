#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int ncc_comptime_dep_value(void);

[[n00b::comptime]] int answer = 42;

int
comptime_main(int argc, char **argv, char **envp)
{
    if (argc != 3 || !argv || !argv[1] || !argv[2]) {
        return 2;
    }

    int saw_env = 0;
    for (char **p = envp; p && *p; p++) {
        if (strncmp(*p, "NCC_CT_PHASE5_ENV=present", 25) == 0) {
            saw_env = 1;
            break;
        }
    }

    const char *env = getenv("NCC_CT_PHASE5_ENV");
    FILE       *f   = fopen(argv[1], "a");
    if (!f) {
        return 3;
    }

    fprintf(f, "argc=%d argv2=%s env=%s envp=%d answer=%d\n",
            argc, argv[2], env ? env : "", saw_env, answer);
    fclose(f);

    return strcmp(argv[2], "alpha") == 0 && env && saw_env ? 0 : 4;
}

int
main(void)
{
    return answer == 42 && ncc_comptime_dep_value() == 9 ? 0 : 5;
}
