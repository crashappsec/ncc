// test_nullable_bind.c — nullability polish #3: binding the result of a
// `?`-returning function to a non-`?` variable silently drops the nullability,
// so it is reported. Both an initializer (`int *y = mk();`) and a plain
// assignment (`z = mk();`) are covered. Declaring the target `?` (as in
// `keep`) suppresses the warning.

extern ?int *mk(void);

int
drop_on_decl(void)
{
    int *y = mk(); // WARN: nullable result of 'mk()' assigned to non-nullable 'y'
    if (y) {
        return *y;
    }
    return 0;
}

int
drop_on_assign(int *z)
{
    z = mk(); // WARN: nullable result of 'mk()' assigned to non-nullable 'z'
    if (z) {
        return *z;
    }
    return 0;
}

int
keep(void)
{
    ?int *y = mk(); // OK: target is declared `?`
    if (y) {
        return *y;
    }
    return 0;
}
