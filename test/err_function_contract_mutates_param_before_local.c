// err_function_contract_mutates_param_before_local - a later
// contract-local declaration does not make earlier parameter mutation safe.

int
mutates_param_before_local(int x)
    requires {
        x = 1;
        int x = 0;
        x == 0;
    }
{
    return x;
}

int
main(void)
{
    return mutates_param_before_local(1);
}
