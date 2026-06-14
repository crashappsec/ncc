// test_nullable_interproc.c — stage 3: interprocedural nullability.
// A `?`-returning call taints its result, and passing a possibly-null value to
// a non-`?` parameter is reported.

?int *mk(void);     // returns nullable
int   need(int *p); // p is non-nullable

int
call_bad(void)
{
    ?int *x = mk();  // x is nullable (mk returns ?)
    return need(x);  // WARN: passing possibly-null 'x' to non-nullable param
}
