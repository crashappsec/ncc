// test_try.c — behavioral test for `_try` error propagation.
//
// Uses a _generic_struct result type with the same shape as n00b_result_t
// (.is_ok / .ok / .err). Verifies that `_try` unwraps an ok result to its
// value, and on an error result makes the enclosing function return that error.

#define RES(T) _generic_struct typeid("result", T) { int is_ok; T ok; int err; }

static RES(int)
ok_(int v)
{
    return (RES(int)){.is_ok = 1, .ok = v, .err = 0};
}

static RES(int)
bad(int e)
{
    return (RES(int)){.is_ok = 0, .ok = 0, .err = e};
}

// Unwrap path: _try yields the .ok value.
static RES(int)
doubler(int x)
{
    int v = _try ok_(x);
    return ok_(v * 2);
}

// Propagation path: the error result is returned from the enclosing function;
// the code after the failing _try does not run.
static RES(int)
propagate(int x)
{
    int v = _try bad(42);
    return ok_(v * 2);
}

int
main(void)
{
    RES(int) a = doubler(5);
    if (!a.is_ok || a.ok != 10) {
        return 1;
    }

    RES(int) b = propagate(5);
    if (b.is_ok || b.err != 42) {
        return 2;
    }

    return 0;
}
