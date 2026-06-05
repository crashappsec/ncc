// err_function_contract_result_shadow - contract locals may not shadow the
// generated ensures result value.

int
result_shadow(int x)
    ensures {
        int result = 0;
        result == x;
    }
{
    return x;
}

int
main(void)
{
    return result_shadow(1);
}
