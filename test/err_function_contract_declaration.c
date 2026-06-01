// err_function_contract_declaration — contracts are definition-only in
// Phase 1, so prototype/declaration syntax must fail through the parser.

int declared_contract(int x)
    requires { x > 0; };

int
main(void)
{
    return 0;
}
