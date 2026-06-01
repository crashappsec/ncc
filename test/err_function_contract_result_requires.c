// err_function_contract_result_requires - `result` is not available in
// precondition contracts.

int
result_in_requires(int x)
    requires { result > x; }
{
    return x + 1;
}

int
main(void)
{
    return result_in_requires(1);
}
