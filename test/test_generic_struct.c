// Test _generic_struct ncc transform.
//
// Compiled and run through ncc (compile_run mode).

#include <stdio.h>
#include <assert.h>
#include <string.h>

// ============================================================================
// Duplicate definitions (same tag, same body) — should deduplicate.
// ============================================================================

_generic_struct gs_point {
    int x;
    int y;
};

_generic_struct gs_point {
    int x;
    int y;
};

// ============================================================================
// A second, distinct generic struct.
// ============================================================================

_generic_struct gs_color {
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

// ============================================================================
// Bare reference (no body) — just becomes `struct gs_point`.
// ============================================================================

static void
use_bare_reference(_generic_struct gs_point *p)
{
    p->x = 100;
    p->y = 200;
}

// ============================================================================
// Use as variable type.
// ============================================================================

static void
test_variable_type(void)
{
    _generic_struct gs_point p = {.x = 1, .y = 2};
    assert(p.x == 1);
    assert(p.y == 2);
    printf("PASS: variable type\n");
}

// ============================================================================
// Use as pointer type.
// ============================================================================

static void
test_pointer_type(void)
{
    struct gs_point p = {.x = 10, .y = 20};
    _generic_struct gs_point *ptr = &p;
    assert(ptr->x == 10);
    assert(ptr->y == 20);
    printf("PASS: pointer type\n");
}

// ============================================================================
// Use as function parameter.
// ============================================================================

static int
sum_point(_generic_struct gs_point p)
{
    return p.x + p.y;
}

static void
test_function_param(void)
{
    struct gs_point p = {.x = 3, .y = 7};
    assert(sum_point(p) == 10);
    printf("PASS: function parameter\n");
}

// ============================================================================
// Multiple distinct generic structs.
// ============================================================================

static void
test_multiple_structs(void)
{
    _generic_struct gs_point  p = {.x = 5, .y = 6};
    _generic_struct gs_color  c = {.r = 255, .g = 128, .b = 0};

    assert(p.x == 5 && p.y == 6);
    assert(c.r == 255 && c.g == 128 && c.b == 0);
    printf("PASS: multiple structs\n");
}

// ============================================================================
// Bare reference used in a function.
// ============================================================================

static void
test_bare_reference(void)
{
    struct gs_point p = {.x = 0, .y = 0};
    use_bare_reference(&p);
    assert(p.x == 100);
    assert(p.y == 200);
    printf("PASS: bare reference\n");
}

// ============================================================================
// Third duplicate at file scope — still deduplicates.
// ============================================================================

_generic_struct gs_point {
    int x;
    int y;
};

static void
test_third_duplicate(void)
{
    _generic_struct gs_point p = {.x = 77, .y = 88};
    assert(p.x == 77 && p.y == 88);
    printf("PASS: third duplicate\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(void)
{
    test_variable_type();
    test_pointer_type();
    test_function_param();
    test_multiple_structs();
    test_bare_reference();
    test_third_duplicate();

    printf("All generic_struct tests passed!\n");
    return 0;
}
