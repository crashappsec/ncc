// err_function_contract_mutates_member - member writes are not
// contract-local mutation.

struct contract_member_target {
    int value;
};

int
mutates_member(struct contract_member_target item)
    requires {
        item.value = 1;
        item.value > 0;
    }
{
    return item.value;
}

int
main(void)
{
    struct contract_member_target item = {.value = 1};
    return mutates_member(item);
}
