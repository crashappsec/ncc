// test_function_contracts_parse — parser-level admission for Phase 1
// function-contract syntax. This file is exercised with `ncc -E` only;
// contract lowering and runtime behavior are later-phase work.

static int
plain_requires(int x)
    requires { x > 0; }
{
    return x;
}

static int
plain_ensures(int x)
    ensures { x >= 0; }
{
    return x;
}

static int
requires_then_ensures(int x)
    requires { x > 0; }
    ensures { x >= 1; }
{
    return x;
}

static int
kargs_then_contracts(int x, +)
    _kargs { int bias = 1; }
    requires { x >= 0; }
    ensures { x + bias >= x; }
{
    return x + bias;
}
