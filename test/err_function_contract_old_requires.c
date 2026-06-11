// err_function_contract_old_requires - old(...) is meaningless in a
// precondition: at entry, the entry state IS the current state.

int
old_in_requires(int x)
    requires { old(x) == x; }
{
    return x;
}

int
main(void)
{
    return old_in_requires(1);
}
