int
shaped(int x)
    requires { x > 0; x < 1000; }
    ensures { result > x; result < 1000; }
{
    return x + 1;
}

void
shaped_void(int *out, int value)
    requires { out != nullptr; }
    ensures { *out == value; }
{
    *out = value;
}

int
apply_fn(int (*fn)(int), int x)
    requires { fn != nullptr; }
    ensures { result >= x; }
{
    return fn(x);
}

int
first_value(int values[3])
    requires { values != nullptr; }
    ensures { result == values[0]; }
{
    return values[0];
}

int
shaped_kargs(int x)
    _kargs { int bias = 1; }
    requires { bias >= 0; }
    ensures { result == x + bias; }
{
    return x + bias;
}

__extension__ int
extension_contract(int x)
    requires { x > 0; }
    ensures { result == x + 4; }
{
    return x + 4;
}

int
support_only_contract(int x)
    requires {
        int support_shadow = x + 1;
    }
{
    return x;
}
