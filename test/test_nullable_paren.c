// test_nullable_paren.c — nullability polish #1: a dereference wrapped in
// balanced parentheses (`*(p)`, `(p)->x`) must still be recognized as a deref
// of the nullable pointer. The only possible warning in this file comes from
// the parenthesized derefs, so if paren-stripping regresses the expected
// stderr substring disappears and the test fails.

struct s {
    int x;
};

int
star_paren(?int *p)
{
    return *(p); // WARN: possible null dereference of nullable 'p'
}

int
arrow_paren(?struct s *p)
{
    return (p)->x; // WARN: possible null dereference of nullable 'p'
}
