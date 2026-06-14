// test_static_typedef_return.c — regression for the declaration-specifier
// ambiguity where a typedef-name return type after a storage-class (or alone)
// parsed as a function_specifier instead of a type_specifier.
//
// The mis-parse is invisible to plain compilation (ncc re-emits the tokens),
// but corrupts any feature that reads the return type. A function contract
// builds a wrapper whose return type is the function's return type, so if
// `box_t` is mis-parsed the wrapper becomes `int make(...)` and the body's
// `return (box_t){...}` fails to compile. With the fix the wrapper is
// `box_t make(...)`.

typedef struct {
    int x;
} box_t;

static box_t
make(int v)
    ensures { result.x == v; }
{
    return (box_t){v};
}

int
main(void)
{
    return make(42).x == 42 ? 0 : 1;
}
