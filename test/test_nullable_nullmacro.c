// test_nullable_nullmacro.c — nullability polish #2: assigning the NULL macro
// (which the preprocessor expands to `((void *)0)`) must mark the target
// nullable again. The deref below is plain (`*p`), so the only way the expected
// warning appears is if `p = NULL;` is recognized as a null assignment. If that
// regresses, `p` is treated as non-null and no warning is emitted.

#define NULL ((void *)0)

int
reassign_null(?int *p)
{
    if (!p) {
        return 0; // p is non-null past here ...
    }
    p = NULL;    // ... until reset to NULL
    return *p;   // WARN: possible null dereference of nullable 'p'
}
