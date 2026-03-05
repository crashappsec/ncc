#include <stdio.h>

// Forward declaration with kargs
int add(int x, int y) _kargs { int bias = 0; bool verbose = false; };

// Definition with kargs (same defaults)
int add(int x, int y) _kargs { int bias = 0; bool verbose = false; } {
    if (verbose) printf("add(%d, %d, bias=%d)\n", x, y, bias);
    return x + y + bias;
}

int main(void) {
    int r1 = add(1, 2);                             // defaults: bias=0, verbose=false
    int r2 = add(1, 2, .bias = 10);                 // override bias
    int r3 = add(1, 2, .verbose = true, .bias = 5); // both overridden
    printf("%d %d %d\n", r1, r2, r3);               // 3 13 8
    return (r1 != 3 || r2 != 13 || r3 != 8);
}
