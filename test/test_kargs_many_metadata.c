// Regression: _kargs metadata must not be capped by a small fixed table.
//
// The transform tracks per-function keyword metadata so later call-site
// rewrites know whether to synthesize a kargs object. A translation unit with
// enough _kargs functions used to fail with "function metadata table full".

#include <stdio.h>

#define DEF_ONE(name)                                                        \
    int name(int x) _kargs { int bias = 1; };                                \
    int name(int x) _kargs { int bias = 1; } { return x + kargs->bias; }

#define CHECK_ONE(name)                                                      \
    do {                                                                     \
        if (name(1) != 2) {                                                  \
            printf("FAIL " #name " default\n");                            \
            return 1;                                                        \
        }                                                                    \
        if (name(1, .bias = 3) != 4) {                                       \
            printf("FAIL " #name " override\n");                           \
            return 1;                                                        \
        }                                                                    \
    } while (0);

#define EACH_10(M, p)                                                        \
    M(p##0) M(p##1) M(p##2) M(p##3) M(p##4)                                 \
    M(p##5) M(p##6) M(p##7) M(p##8) M(p##9)

#define EACH_100(M, p)                                                       \
    EACH_10(M, p##0) EACH_10(M, p##1) EACH_10(M, p##2) EACH_10(M, p##3)      \
    EACH_10(M, p##4) EACH_10(M, p##5) EACH_10(M, p##6) EACH_10(M, p##7)      \
    EACH_10(M, p##8) EACH_10(M, p##9)

#define EACH_300(M) EACH_100(M, f0) EACH_100(M, f1) EACH_100(M, f2)

EACH_300(DEF_ONE)

int main(void) {
    EACH_300(CHECK_ONE)
    printf("ok\n");
    return 0;
}
