// err_function_contract_goto - contract blocks cannot jump into or out of
// the enclosing function.

int
goto_in_contract(int x)
    requires {
        goto done;
        x > 0;
    }
{
done:
    return x;
}

int
main(void)
{
    return goto_in_contract(1);
}
