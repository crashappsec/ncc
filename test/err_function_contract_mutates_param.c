// err_function_contract_mutates_param - parameters are not contract-local.

int
mutates_param(int x)
    requires {
        x = 1;
        x > 0;
    }
{
    return x;
}

int
main(void)
{
    return mutates_param(1);
}
