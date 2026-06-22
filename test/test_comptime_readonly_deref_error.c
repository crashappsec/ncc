struct S {
    int x;
};
[[n00b::comptime]] struct S *root = 0;

int
main(void)
{
    (*root).x = 7;
    return root != 0;
}
