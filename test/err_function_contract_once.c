// err_function_contract_once — Phase 2 rejects _Once plus contracts
// explicitly. Full composition is deferred to Phase 4.

_Once int
once_contract(int x)
    requires { x > 0; }
{
    return x;
}

int
main(void)
{
    return once_contract(1);
}
