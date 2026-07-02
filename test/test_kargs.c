#include <stdio.h>

// Forward declaration with kargs
int add(int x, int y) _kargs { int bias = 0; bool verbose = false; };

// Definition with kargs (same defaults)
int add(int x, int y) _kargs { int bias = 0; bool verbose = false; } {
    if (verbose) printf("add(%d, %d, bias=%d)\n", x, y, bias);
    return x + y + bias;
}

// Regression: a C23 attribute prefix on a _kargs forward declaration
// must not break parsing (crashappsec/ncc#9).
[[deprecated("regression-only")]] int sub(int x, int y) _kargs { int bias = 0; };

int sub(int x, int y) _kargs { int bias = 0; } {
    return x - y - bias;
}

static int plain_default_counter = 0;

static int
next_plain_default(void)
{
    plain_default_counter++;
    return 30 + plain_default_counter;
}

int default_once(int x) _kargs { int bias = next_plain_default(); } {
    return x + bias;
}

int nested_inner(int x) _kargs { int bias = 0; } {
    return x + bias;
}

int nested_outer(int x) _kargs { int suffix = 0; } {
    return x + suffix;
}

int main(void) {
    int r1 = add(1, 2);                             // defaults: bias=0, verbose=false
    int r2 = add(1, 2, .bias = 10);                 // override bias
    int r3 = add(1, 2, .verbose = true, .bias = 5); // both overridden
    printf("%d %d %d\n", r1, r2, r3);               // 3 13 8
    int r4 = sub(10, 3, .bias = 2);                 // 5
    if (r1 != 3 || r2 != 13 || r3 != 8 || r4 != 5) {
        return 1;
    }

    plain_default_counter = 0;
    if (default_once(5) != 36) {
        return 2;
    }
    if (plain_default_counter != 1) {
        return 3;
    }

    plain_default_counter = 0;
    if (default_once(5, .bias = 9) != 14) {
        return 4;
    }
    if (plain_default_counter != 0) {
        return 5;
    }

    plain_default_counter = 0;
    if (default_once(5, kw_func(default_once)) != 36) {
        return 6;
    }
    if (plain_default_counter != 1) {
        return 7;
    }

    plain_default_counter = 0;
    if (default_once(5, kw_func(default_once, .bias = 11)) != 16) {
        return 8;
    }
    if (plain_default_counter != 0) {
        return 9;
    }

    if (nested_outer(nested_inner(7)) != 7) {
        return 10;
    }

    if (nested_outer(nested_inner(7, .bias = 2), .suffix = 3) != 12) {
        return 11;
    }

    return 0;
}
