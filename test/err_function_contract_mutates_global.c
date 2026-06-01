// err_function_contract_mutates_global - file-scope state is not
// contract-local.

static int global_value;

int
mutates_global(int x)
    requires {
        global_value++;
        x > 0;
    }
{
    return x;
}

int
main(void)
{
    return mutates_global(1);
}
