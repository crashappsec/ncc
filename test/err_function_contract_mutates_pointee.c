// err_function_contract_mutates_pointee - v1 rejects pointed-to mutation.

int
mutates_pointee(int *out)
    requires {
        *out = 1;
        out != nullptr;
    }
{
    return *out;
}

int
main(void)
{
    int value = 0;
    return mutates_pointee(&value);
}
