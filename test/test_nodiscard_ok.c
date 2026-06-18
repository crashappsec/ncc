// test_nodiscard_ok.c — result returns that ARE handled must not warn. Covers
// every consuming context plus the `(void)` opt-out and a non-result call, so a
// false positive in the must-check pass turns the "is ignored" diagnostic on
// and fails the test (preprocess_stderr_omits).

#define RES(T) _generic_struct typeid("result", T) { int is_ok; T ok; int err; }

static RES(int)
mk(int v)
{
    return (RES(int)){.is_ok = 1, .ok = v, .err = 0};
}

static int
plain(int v)
{
    return v;
}

static RES(int)
via_try(void)
{
    int v = _try mk(7); // consumed by _try (checks + unwraps)
    return mk(v);
}

int
main(void)
{
    (void)mk(1);            // explicit opt-out
    RES(int) r = mk(2);     // consumed: declaration initializer
    plain(4);               // not a result type — never must-check
    r = mk(5);              // consumed: assignment
    if (mk(6).is_ok) {      // consumed: member access
    }
    RES(int) t = via_try(); // consumed
    return (r.is_ok && t.is_ok) ? 0 : 1;
}
