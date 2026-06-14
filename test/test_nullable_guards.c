// test_nullable_guards.c — nullability polish: refinements that must NOT warn.
// Every function below dereferences a `?` pointer only on a path where it has
// been proven non-null, so the analyzer must stay silent. The test asserts that
// no "null dereference" diagnostic appears on stderr (preprocess_stderr_omits);
// if ternary / comparison-guard / short-circuit refinement regresses, one of
// these turns into a false positive and the test fails.

#define NULL ((void *)0)

struct s {
    int x;
};

// #1 ternary: the deref is in the arm guarded by the condition.
int
ternary_then(?int *p)
{
    return p ? *p : 0;
}

// #2 comparison guards: `!= NULL` / `== NULL`, either operand order.
int
ne_guard(?int *p)
{
    if (p != NULL) {
        return *p;
    }
    return 0;
}

int
eq_guard(?int *p)
{
    if (p == NULL) {
        return 0;
    }
    return *p; // reached only when p is non-null
}

int
ne_reversed(?int *p)
{
    if (NULL != p) {
        return *p;
    }
    return 0;
}

// #3 short-circuit: && evaluates the RHS only when the LHS is true; || only
// when the LHS is false.
int
and_guard(?struct s *p)
{
    return p && p->x;
}

int
or_guard(?int *p)
{
    return !p || *p;
}

// Guards also refine the controlled block body.
int
and_block(?int *p)
{
    if (p != NULL) {
        return *p;
    }
    return 0;
}
