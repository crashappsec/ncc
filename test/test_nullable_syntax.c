// test_nullable_syntax.c — stage 1 of nullability: the `?` nullable qualifier
// parses in every type position, is stripped before emit (clang never sees it),
// and does not disturb the ternary operator.

?int *gp; // nullable global pointer

static int
deref(?int *p) // nullable parameter
{
    return *p;
}

static ?char * // nullable return
maybe(int x)
{
    return x ? "hi" : (char *)0; // ternary still works
}

int
main(void)
{
    int v = 5;
    if (deref(&v) != 5) {
        return 1;
    }

    ?char *q = maybe(1); // nullable local
    if (!q) {
        return 2;
    }

    ?char *n = maybe(0);
    return n ? 3 : 0; // ternary on a nullable value
}
