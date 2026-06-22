struct S {
    int x;
};
[[n00b::comptime]] struct S state = { 0 };

int
main(void)
{
    state.x = 7;
    return state.x;
}
