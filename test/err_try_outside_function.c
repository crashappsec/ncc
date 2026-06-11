// err_try_outside_function - `_try` propagates by returning from the enclosing
// function, so it is only valid inside one.

extern int foo(void);

int x = _try foo();

int
main(void)
{
    return 0;
}
