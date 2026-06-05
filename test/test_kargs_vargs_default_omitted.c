// Regression test for WP-001 Phase 2: omitted keyword defaults must still
// take effect when the callee body reads the keyword through the synthesized
// `kargs` pointer (kargs -> name), not only through the injected body local.
//
// Phase 2 stopped emitting omitted defaults at the call site (so each default
// expression is evaluated exactly once, in the callee — D-009).  The body
// extraction computes a local `name` from
// `kargs -> _has_name ? kargs -> name : ( default )` and writes the resolved
// value back into `kargs -> name`, so both access styles observe the default.
// Before the fix, an omitted defaulted keyword read through `kargs -> name`
// came through as a zero-initialized field (false / 0) instead of its
// declared default.  This reproduced the downstream libn00b failure
// (test_construct_record_ordered: `ordered` defaulting to true).
//
// Each function shape that the W2 write-back guard protects is covered
// EXPLICITLY below, and every body reads the keyword DIRECTLY through
// `kargs -> field` (the pathway that regressed):
//   (a) kargs-only function.
//   (b) kargs + vargs function.
//   (c) the kw_func(...) call form with the keyword omitted.
// Each case asserts: the default is applied when omitted, an explicit
// override is honored, and a side-effecting default is evaluated exactly once.

#include <stdio.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

// ---------------------------------------------------------------------------
// (a) kargs-only function, body reads kargs -> field directly.
// `enabled` defaults to true (non-zero, so a zero-initialized field would be
// visibly wrong) and `weight` defaults to 42.
// ---------------------------------------------------------------------------

int a_kargs_only(int base) _kargs { bool enabled = true; int weight = 42; };
int a_kargs_only(int base) _kargs { bool enabled = true; int weight = 42; } {
    // Deliberately read through the kargs pointer, NOT the body locals.
    return base + (kargs->enabled ? kargs->weight : 0);
}

// ---------------------------------------------------------------------------
// (b) kargs + vargs function, body reads kargs -> field directly.
// ---------------------------------------------------------------------------

int b_kargs_vargs(int base, +) _kargs { bool ordered = true; int weight = 42; };
int b_kargs_vargs(int base, +) _kargs { bool ordered = true; int weight = 42; } {
    int contribution = kargs->ordered ? kargs->weight : 0;
    return base + (int)vargs->nargs + contribution;
}

// ---------------------------------------------------------------------------
// (c) is exercised below against a_kargs_only and b_kargs_vargs via the
// kw_func(...) literal call form; no separate function is needed.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Single-evaluation: a side-effecting default must run exactly once when the
// keyword is omitted, and never when it is overridden (D-009).  Body reads the
// resolved value through kargs -> field.
// ---------------------------------------------------------------------------

static int c_default_calls = 0;

static int c_next_default(void) {
    c_default_calls++;
    return 100 + c_default_calls;
}

int c_side_effect(int base) _kargs { int amount = c_next_default(); };
int c_side_effect(int base) _kargs { int amount = c_next_default(); } {
    return base + kargs->amount;
}

int main(void) {
    // === (a) kargs-only ====================================================
    // Default applied (omitted): enabled=true, weight=42 -> 1 + 42 = 43.
    if (a_kargs_only(1) != 43) {
        printf("FAIL (a) default not applied via kargs->field\n");
        return 1;
    }
    // Explicit override honored: weight=5 -> 1 + 5 = 6.
    if (a_kargs_only(1, .weight = 5) != 6) {
        printf("FAIL (a) explicit override not honored\n");
        return 2;
    }
    // Override enabled=false -> contribution 0 -> 1.
    if (a_kargs_only(1, .enabled = false) != 1) {
        printf("FAIL (a) bool override not honored\n");
        return 3;
    }

    // === (b) kargs + vargs =================================================
    // Default applied (keyword omitted): base=1, 2 vargs, contribution=42 ->
    // 45.  A vargs+kargs call with the keyword omitted must spell the omission
    // with kw_func(...): a bare positional call cannot be distinguished from
    // an already-transformed call (the trailing positionals would be mistaken
    // for the synthesized vargs/kargs args), so kw_func is the omitted form.
    if (b_kargs_vargs(1, 10, 20, kw_func(b_kargs_vargs)) != 45) {
        printf("FAIL (b) default not applied via kargs->field\n");
        return 4;
    }
    // Explicit override honored: weight=5, base=2, 1 varg -> 2+1+5 = 8.
    if (b_kargs_vargs(2, 99, .weight = 5) != 8) {
        printf("FAIL (b) explicit override not honored\n");
        return 5;
    }
    // Override ordered=false: contribution 0, base=3, 0 vargs -> 3.
    if (b_kargs_vargs(3, .ordered = false) != 3) {
        printf("FAIL (b) bool override not honored\n");
        return 6;
    }

    // === (c) kw_func(...) call form ========================================
    // kw_func with keyword OMITTED on kargs+vargs: defaults apply -> 45.
    if (b_kargs_vargs(1, 10, 20, kw_func(b_kargs_vargs)) != 45) {
        printf("FAIL (c) kw_func default not applied\n");
        return 7;
    }
    // kw_func with explicit override honored: weight=5, base=2, 1 varg -> 8.
    if (b_kargs_vargs(2, 99, kw_func(b_kargs_vargs, .weight = 5)) != 8) {
        printf("FAIL (c) kw_func explicit override not honored\n");
        return 8;
    }
    // kw_func on kargs-only with keyword omitted: defaults apply -> 43.
    if (a_kargs_only(1, kw_func(a_kargs_only)) != 43) {
        printf("FAIL (c) kw_func kargs-only default not applied\n");
        return 9;
    }

    // === single-evaluation =================================================
    // Omitted: default runs exactly once. base=1, amount=101 -> 102.
    c_default_calls = 0;
    if (c_side_effect(1) != 102) {
        printf("FAIL single-eval value\n");
        return 10;
    }
    if (c_default_calls != 1) {
        printf("FAIL single-eval call count=%d (expected 1)\n", c_default_calls);
        return 11;
    }
    // Override: default runs zero times. base=1, amount=9 -> 10.
    c_default_calls = 0;
    if (c_side_effect(1, .amount = 9) != 10) {
        printf("FAIL single-eval override value\n");
        return 12;
    }
    if (c_default_calls != 0) {
        printf("FAIL single-eval override call count=%d (expected 0)\n",
               c_default_calls);
        return 13;
    }
    // Omitted via kw_func: default runs exactly once. base=2 -> 103.
    c_default_calls = 0;
    if (c_side_effect(2, kw_func(c_side_effect)) != 103) {
        printf("FAIL single-eval kw_func value\n");
        return 14;
    }
    if (c_default_calls != 1) {
        printf("FAIL single-eval kw_func call count=%d (expected 1)\n",
               c_default_calls);
        return 15;
    }

    printf("ok\n");
    return 0;
}
