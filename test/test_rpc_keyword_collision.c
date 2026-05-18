// test_rpc_keyword_collision — verify the contextual-keyword lookahead
// preserves `rpc` as a plain identifier in every position user code
// might use it (variable, struct field, function name, parameter,
// expression).
//
// This guards against the `@rpc(...)` clause's terminal registration
// regressing existing C source. The companion @rpc tests cover the
// *keyword* path; this test covers the *identifier* path.

#include <stdio.h>

static int local_var(void)
{
    int rpc = 42;
    return rpc;
}

struct widget {
    int rpc;
    int extra;
};

static int field_use(void)
{
    struct widget w = {.rpc = 7, .extra = 3};
    return w.rpc + w.extra;
}

static void rpc(int x);
static void
rpc(int x)
{
    (void)x;
}

static int
param_use(int rpc)
{
    return rpc * 2;
}

static int
expr_use(void)
{
    int rpc = 1;
    return (rpc + 2) * 3;
}

static int
pointer_use(void)
{
    int  rpc = 11;
    int *p   = &rpc;
    return *p;
}

int
main(void)
{
    if (local_var() != 42) {
        return 1;
    }

    if (field_use() != 10) {
        return 2;
    }

    rpc(5);

    if (param_use(21) != 42) {
        return 3;
    }

    if (expr_use() != 9) {
        return 4;
    }

    if (pointer_use() != 11) {
        return 5;
    }

    printf("OK\n");
    return 0;
}
