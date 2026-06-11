// err_defer_escape - control may not be transferred out of a _defer body.

int
f(void)
{
    _defer { return 1; }
    return 0;
}

int
main(void)
{
    return f();
}
