// test_typehash_expr.c — typehash of an EXPRESSION resolves to the typehash of
// the expression's inferred type (via the scoped symbol table + expression
// typer). typehash(expr) must equal typehash(<the type of expr>), byte for
// byte, since the type model recomputes the canonical spelling identically.
//
// Restricted to non-pointer RESULT types so the run compiles standalone: a
// pointer typehash emits a GC-map descriptor that pulls in n00b runtime types
// (those paths are covered by the preprocess-mode gc-typemap tests). Pointer
// and address-of forms are exercised by the inference unit coverage.

#include <stdint.h>

static int  gi;
static int *gp;

int
main(void)
{
    int ok = 1;

    // Identifier: type of `gi` is `int`.
    ok &= (typehash(gi) == typehash(int));

    // Dereference: type of `*gp` is `int`.
    ok &= (typehash(*gp) == typehash(int));

    // Parenthesized: type of `(gi)` is `int`.
    ok &= (typehash((gi)) == typehash(int));

    // Cast: type of `(char)gi` is `char`.
    ok &= (typehash((char)gi) == typehash(char));

    // typestr(expr) spells the inferred type, matching typestr(<that type>).
    ok &= (__builtin_strcmp(typestr(gi), typestr(int)) == 0);
    ok &= (__builtin_strcmp(typestr(*gp), typestr(int)) == 0);

    return ok ? 0 : 1;
}
