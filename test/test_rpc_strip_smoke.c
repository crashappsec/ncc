// test_rpc_strip_smoke — Phase B regression: an `@rpc(...)` annotated
// function must compile and run cleanly even though Phase B does not
// yet synthesize the dispatcher / ctor / client stub. The xform's
// job here is just to recognize the clause, validate the method
// string, and strip the clause so clang sees plain C.
//
// Uses simple `int (int)` signatures intentionally — parametric type
// support (`n00b_result_t(T *)`, `n00b_rpc_stream_t(T) *`) lands with
// the unary end-to-end test in a later phase.

#include <stdio.h>

int hello(int x) @rpc("foo.v1.Foo/Bar");

int
hello(int x) @rpc("foo.v1.Foo/Bar")
{
    return x + 1;
}

int
main(void)
{
    if (hello(41) != 42) {
        return 1;
    }

    printf("OK\n");
    return 0;
}
