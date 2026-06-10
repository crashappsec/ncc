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

typedef struct tpe_point {
    int  x;
    long y;
} tpe_point_t;

static int          gi;
static int         *gp;
static tpe_point_t  gs;
static tpe_point_t *gpp;

// Prototypes for call typing (never actually called — typehash is a
// compile-time type query — so no definition is needed).
int         tpe_id_fn(int);
tpe_point_t tpe_mk_fn(void);

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

    // Member access: `s.field` (first and later members) and `p->field`.
    ok &= (typehash(gs.x) == typehash(int));
    ok &= (typehash(gs.y) == typehash(long));
    ok &= (typehash(gpp->x) == typehash(int));
    ok &= (typehash(gpp->y) == typehash(long));

    // Function call: type of `f(args)` is f's return type.
    ok &= (typehash(tpe_id_fn(0)) == typehash(int));

    // Member access on a call result: type of `mk().y` is the field type.
    ok &= (typehash(tpe_mk_fn().y) == typehash(long));

    return ok ? 0 : 1;
}
