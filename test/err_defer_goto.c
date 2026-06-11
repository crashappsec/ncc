// err_defer_goto - a function using _defer may not (yet) also use goto;
// running the right defers across the jump needs label-scope analysis.

int
f(int x)
{
    _defer { x = 0; }
    goto done;
done:
    return x;
}

int
main(void)
{
    return f(1);
}
