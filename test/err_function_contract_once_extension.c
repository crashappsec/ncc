// err_function_contract_once_extension — _Once plus contracts remains
// explicitly rejected when wrapped in the C __extension__ function form.

__extension__ _Once int
once_extension_contract(int x)
    requires { x > 0; }
{
    return x;
}

int
main(void)
{
    return once_extension_contract(1);
}
