// test_function_contract_keyword_collision — verify Phase 1 contract
// terminals do not reserve `requires` or `ensures` as ordinary C names.

struct contract_words {
    int requires;
    int ensures;
};

static int requires(int ensures);
static int ensures(int requires);

static int
requires(int ensures)
{
    return ensures + 1;
}

static int
ensures(int requires)
{
    return requires + 2;
}

static int
local_var_use(void)
{
    int requires = 3;
    int ensures  = 4;
    return requires + ensures;
}

static int
field_use(void)
{
    struct contract_words words = {.requires = 5, .ensures = 6};
    return words.requires + words.ensures;
}

static int
param_use(int requires, int ensures)
{
    return requires * ensures;
}

static int
expr_use(void)
{
    int requires = 7;
    int ensures  = 8;
    return (requires + ensures) - requires;
}

int
main(void)
{
    if (local_var_use() != 7) {
        return 1;
    }

    if (field_use() != 11) {
        return 2;
    }

    if (requires(9) != 10) {
        return 3;
    }

    if (ensures(9) != 11) {
        return 4;
    }

    if (param_use(3, 4) != 12) {
        return 5;
    }

    if (expr_use() != 8) {
        return 6;
    }

    return 0;
}
