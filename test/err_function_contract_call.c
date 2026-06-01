// err_function_contract_call - v1 function contracts reject calls.

static int
helper(int x)
{
    return x + 1;
}

int
call_in_contract(int x)
    requires { helper(x) > 0; }
{
    return x;
}

int
main(void)
{
    return call_in_contract(1);
}
