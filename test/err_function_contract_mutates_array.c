// err_function_contract_mutates_array - array element writes are not
// contract-local mutation.

int
mutates_array(int values[3])
    requires {
        values[0] = 1;
        values[0] > 0;
    }
{
    return values[0];
}

int
main(void)
{
    int values[3] = {1, 2, 3};
    return mutates_array(values);
}
