// test_typehash_autotag.c — expression typing must handle two real-world C
// patterns the type model previously missed:
//
//   1. `auto v = init;` — infer v's type from its initializer.
//   2. `typedef struct X X;` — a tag whose name coincides with a typedef of the
//      same name (ubiquitous in C / n00b). The tag name tokenizes as a
//      TYPEDEF_NAME, so it must still be recorded so members resolve.
//
// Both are required for typeof(*p)/typehash through the standard n00b container
// macros (which bind `auto _lp = &(list)` and use same-name typedef'd structs).
// Non-pointer result types keep the run standalone.

typedef struct atn_node atn_node;   // same-name typedef (tag == typedef)
struct atn_node {
    int       v;
    long      w;
    atn_node *next;
};

static int
use(atn_node *p)
{
    int ok = 1;

    // auto var inferred from a parameter pointer.
    auto q = p;
    ok &= (typehash(q->v) == typehash(int));
    ok &= (typehash(q->w) == typehash(long));

    // auto var inferred from a member-access expression.
    auto m = p->v;
    ok &= (typehash(m) == typehash(int));

    // typeof through the same-name-typedef tag.
    ok &= (typehash(typeof(*p)) == typehash(atn_node));
    ok &= (__builtin_strcmp(typestr(typeof(p->w)), typestr(long)) == 0);

    return ok;
}

int
main(void)
{
    atn_node n = {.v = 1, .w = 2, .next = (void *)0};
    return use(&n) ? 0 : 1;
}
