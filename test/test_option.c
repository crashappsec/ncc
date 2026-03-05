// Test _option(T) ncc transform.
//
// Compiled and run through ncc (compile_run mode).

#include <stdio.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Value type: _option(int)
// ============================================================================

static void
test_value_type_some(void)
{
    _option(int) x = _some(int, 42);
    assert(_is_some(x));
    assert(!_is_none(x));
    assert(_unwrap(x) == 42);
    printf("PASS: value type _some\n");
}

static void
test_value_type_none(void)
{
    _option(int) y = _none(int);
    assert(_is_none(y));
    assert(!_is_some(y));
    printf("PASS: value type _none\n");
}

static void
test_value_type_sizeof(void)
{
    // Value option should be larger than bare int (has_value + value).
    assert(sizeof(_option(int)) > sizeof(int));
    printf("PASS: value type sizeof\n");
}

static void
test_value_type_reassign(void)
{
    _option(int) z = _none(int);
    assert(_is_none(z));
    z = _some(int, 99);
    assert(_is_some(z));
    assert(_unwrap(z) == 99);
    printf("PASS: value type reassign\n");
}

// ============================================================================
// Pointer type: _option(int *)
// ============================================================================

static void
test_pointer_type_some(void)
{
    int val = 7;
    _option(int *) p = _some(int *, &val);
    assert(_is_some(p));
    assert(!_is_none(p));
    assert(*_unwrap(p) == 7);
    printf("PASS: pointer type _some\n");
}

static void
test_pointer_type_none(void)
{
    _option(int *) q = _none(int *);
    assert(_is_none(q));
    assert(!_is_some(q));
    printf("PASS: pointer type _none\n");
}

static void
test_pointer_type_sizeof(void)
{
    // Pointer option should have same size as bare pointer (no wrapper struct).
    assert(sizeof(_option(int *)) == sizeof(int *));
    printf("PASS: pointer type sizeof\n");
}

// ============================================================================
// Struct pointer type: _option(struct foo *)
// ============================================================================

struct foo {
    int a;
    int b;
};

static void
test_struct_pointer(void)
{
    struct foo f = {.a = 10, .b = 20};
    _option(struct foo *) sp = _some(struct foo *, &f);
    assert(_is_some(sp));
    assert(_unwrap(sp)->a == 10);
    assert(_unwrap(sp)->b == 20);

    _option(struct foo *) sp2 = _none(struct foo *);
    assert(_is_none(sp2));
    printf("PASS: struct pointer type\n");
}

// ============================================================================
// const char * pointer type
// ============================================================================

static void
test_const_char_pointer(void)
{
    _option(const char *) s = _some(const char *, "hello");
    assert(_is_some(s));
    assert(strcmp(_unwrap(s), "hello") == 0);

    _option(const char *) s2 = _none(const char *);
    assert(_is_none(s2));
    printf("PASS: const char * type\n");
}

// ============================================================================
// Double type (non-pointer value)
// ============================================================================

static void
test_double_value(void)
{
    _option(double) d = _some(double, 3.14);
    assert(_is_some(d));
    double v = _unwrap(d);
    assert(v > 3.13 && v < 3.15);

    _option(double) d2 = _none(double);
    assert(_is_none(d2));
    printf("PASS: double value type\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(void)
{
    test_value_type_some();
    test_value_type_none();
    test_value_type_sizeof();
    test_value_type_reassign();
    test_pointer_type_some();
    test_pointer_type_none();
    test_pointer_type_sizeof();
    test_struct_pointer();
    test_const_char_pointer();
    test_double_value();

    printf("All option tests passed!\n");
    return 0;
}
