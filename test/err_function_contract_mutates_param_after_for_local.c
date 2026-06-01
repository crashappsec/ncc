// err_function_contract_mutates_param_after_for_local - for-init locals do
// not escape their loop scope.

int
mutates_param_after_for_local(int x)
    requires {
        for (int x = 0; x < 1; x++) {
        }
        x++;
        x > 0;
    }
{
    return x;
}

int
main(void)
{
    return mutates_param_after_for_local(1);
}
