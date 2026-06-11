// test_typehash_scoped.c — expression typing must resolve LOCALS and PARAMETERS
// (and typeof of expressions built from them), not just file-scope globals.
//
// Regression for the scope-aware typing fix: parameters were never recorded
// (the parameter_type_list nests below the declarator, missed by a group-only
// child search) and nested scopes were freed on pop with root-only lookup, so
// any typehash/typeid/typestr/typeof of a local- or param-based expression
// silently hashed the identifier TEXT instead of the type. Non-pointer result
// types keep the run standalone (a pointer typehash pulls in runtime GC types).

typedef struct tps_point {
    int  x;
    long y;
} tps_point_t;

typedef struct tps_box {
    tps_point_t *p;
    int          n;
} tps_box_t;

static int
use_param(tps_point_t pt, tps_box_t *bx)
{
    int        ok = 1;
    tps_point_t lp;     // local
    tps_box_t  *lb = bx; // local pointer

    // Parameter resolves to its declared type.
    ok &= (typehash(pt) == typehash(tps_point_t));
    ok &= (typehash(pt.x) == typehash(int));
    ok &= (typehash(pt.y) == typehash(long));

    // Local resolves likewise.
    ok &= (typehash(lp) == typehash(tps_point_t));
    ok &= (typehash(lp.y) == typehash(long));

    // Parameter pointer: member access through it.
    ok &= (typehash(bx->n) == typehash(int));
    ok &= (typehash(lb->n) == typehash(int));

    // typeof of an expression built from a parameter/local — the element type
    // recovered from a data pointer, as the generic containers will do.
    ok &= (typehash(typeof(*bx->p)) == typehash(tps_point_t));
    ok &= (typehash(typeof((*bx->p).y)) == typehash(long));

    // typestr spells the resolved type identically to the written type.
    ok &= (__builtin_strcmp(typestr(lp), typestr(tps_point_t)) == 0);
    ok &= (__builtin_strcmp(typestr(typeof(*bx->p)), typestr(tps_point_t)) == 0);

    return ok;
}

int
main(void)
{
    tps_point_t pt = {.x = 1, .y = 2};
    tps_point_t pp = {.x = 3, .y = 4};
    tps_box_t   bx = {.p = &pp, .n = 5};
    return use_param(pt, &bx) ? 0 : 1;
}
