// Regression test for the `(void)`-parameter kargs signature bug (WP-001
// Phase 5 follow-up).
//
// A keyword-argument function whose parameter list is spelled exactly
// `(void)` previously generated invalid C: the kargs parameter was APPENDED
// after the `void`, producing
//     f ( void , struct _f__kargs * kargs )
// which does not compile.  `(void)` means "takes no arguments", so the kargs
// parameter must REPLACE the `void`, exactly as for a genuinely empty
// parameter list, yielding
//     f ( struct _f__kargs * kargs ).
//
// This test FAILS to compile on the pre-fix compiler (so the test fails) and
// compiles + runs on the fixed compiler.  It also reads an omitted default
// through `kargs -> field` to confirm the default still resolves for the
// `(void)` shape.

#include <stdio.h>

// kargs function whose positional parameter list is exactly `(void)`.
// `enabled` defaults to true and `weight` to 42 — both non-zero, so a
// zero-initialized field would be visibly wrong.
int void_kargs(void) _kargs { bool enabled = true; int weight = 42; };
int void_kargs(void) _kargs { bool enabled = true; int weight = 42; } {
    // Read through the kargs pointer (the pathway the omitted-default fix
    // protects) to confirm the default resolves for the `(void)` signature.
    return kargs->enabled ? kargs->weight : 0;
}

int main(void) {
    // Defaults applied (no keyword supplied): enabled=true, weight=42 -> 42.
    if (void_kargs() != 42) {
        printf("FAIL void_kargs() default not applied: got %d\n", void_kargs());
        return 1;
    }

    // Explicit override honored: weight = 7 -> 7.
    if (void_kargs(.weight = 7) != 7) {
        printf("FAIL void_kargs(.weight=7) override not honored\n");
        return 2;
    }

    // Override enabled=false -> 0.
    if (void_kargs(.enabled = false) != 0) {
        printf("FAIL void_kargs(.enabled=false) override not honored\n");
        return 3;
    }

    // kw_func(...) form with the keyword omitted: defaults still apply -> 42.
    if (void_kargs(kw_func(void_kargs)) != 42) {
        printf("FAIL void_kargs(kw_func(...)) default not applied\n");
        return 4;
    }

    printf("ok\n");
    return 0;
}
