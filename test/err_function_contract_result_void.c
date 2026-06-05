// err_function_contract_result_void - void functions do not expose a
// contract `result` value.

void
result_in_void_ensures(int *out)
    ensures { result == 0; }
{
    *out = 0;
}

int
main(void)
{
    int value = 1;
    result_in_void_ensures(&value);
    return value;
}
