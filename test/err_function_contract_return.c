// err_function_contract_return - contract blocks are not function bodies.

int
return_in_contract(int x)
    requires {
        if (x < 0) {
            return 0;
        }
        x > 0;
    }
{
    return x;
}

int
main(void)
{
    return return_in_contract(1);
}
