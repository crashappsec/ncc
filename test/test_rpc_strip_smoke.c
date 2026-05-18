// test_rpc_strip_smoke — verify that an @rpc clause attached to a
// prototype (no body) is stripped from the parent declaration so
// plain clang accepts the result.
//
// This complements test_rpc_unary, which exercises the full
// synthesize-on-function-definition path. The strip-on-prototype
// path runs even when the signature isn't unary (the user may
// attach @rpc to a forward declaration with whatever types they
// want; only the function_definition triggers shape validation +
// synthesis).

#include <stdio.h>

// Prototype with @rpc — gets the clause stripped, then it's a
// plain extern prototype that we never actually call.
extern int hello_proto(int x) @rpc("foo.v1.Foo/Bar");

int
main(void)
{
    printf("OK\n");
    return 0;
}
