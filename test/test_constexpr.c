// test_constexpr.c — Test constexpr transforms.
//
// Compile with ncc, then link with clang and run.
// Each test prints PASS or FAIL.

#include <stdio.h>
#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
    double  z;
} my_point_t;

typedef struct {
    char    name[64];
    int64_t value;
    double  weight;
} my_record_t;

// Test 1: constexpr_eval with sizeof(int) — builtin type.
static void
test_eval_sizeof_int(void)
{
    int result = constexpr_eval(sizeof(int));
    printf("test_eval_sizeof_int: %s (got %d, expected %d)\n",
           result == (int)sizeof(int) ? "PASS" : "FAIL",
           result, (int)sizeof(int));
}

// Test 2: constexpr_eval with sizeof(my_point_t) — user-defined struct.
static void
test_eval_sizeof_struct(void)
{
    int result = constexpr_eval(sizeof(my_point_t));
    printf("test_eval_sizeof_struct: %s (got %d, expected %d)\n",
           result == (int)sizeof(my_point_t) ? "PASS" : "FAIL",
           result, (int)sizeof(my_point_t));
}

// Test 3: constexpr_max with two args.
static void
test_max_two(void)
{
    int result = constexpr_max(sizeof(int), sizeof(long));
    int expected = sizeof(int) > sizeof(long) ? sizeof(int) : sizeof(long);
    printf("test_max_two: %s (got %d, expected %d)\n",
           result == expected ? "PASS" : "FAIL",
           result, expected);
}

// Test 4: constexpr_min with two args.
static void
test_min_two(void)
{
    int result = constexpr_min(sizeof(char), sizeof(int));
    int expected = sizeof(char) < sizeof(int) ? sizeof(char) : sizeof(int);
    printf("test_min_two: %s (got %d, expected %d)\n",
           result == expected ? "PASS" : "FAIL",
           result, expected);
}

// Test 5: constexpr_max with three args.
static void
test_max_three(void)
{
    int result = constexpr_max(sizeof(char), sizeof(int), sizeof(double));
    int expected = sizeof(double);
    printf("test_max_three: %s (got %d, expected %d)\n",
           result == expected ? "PASS" : "FAIL",
           result, expected);
}

// Test 6: constexpr_strlen.
static void
test_strlen(void)
{
    int result = constexpr_strlen("hello");
    printf("test_strlen: %s (got %d, expected 5)\n",
           result == 5 ? "PASS" : "FAIL", result);
}

// Test 7: constexpr_strcmp equal strings.
static void
test_strcmp_equal(void)
{
    int result = constexpr_strcmp("abc", "abc");
    printf("test_strcmp_equal: %s (got %d, expected 0)\n",
           result == 0 ? "PASS" : "FAIL", result);
}

// Test 8: constexpr_strcmp different strings.
static void
test_strcmp_less(void)
{
    int result = constexpr_strcmp("abc", "def");
    printf("test_strcmp_less: %s (got %d, expected <0)\n",
           result < 0 ? "PASS" : "FAIL", result);
}

// Test 9: constexpr_paste with string and integer.
static void
test_paste_str_int(void)
{
    int constexpr_paste("item_", 3) = 42;
    printf("test_paste_str_int: %s (item_3 = %d)\n",
           item_3 == 42 ? "PASS" : "FAIL", item_3);
}

// Test 10: constexpr_paste with two strings.
static void
test_paste_str_str(void)
{
    int constexpr_paste("field_", "name") = 99;
    printf("test_paste_str_str: %s (field_name = %d)\n",
           field_name == 99 ? "PASS" : "FAIL", field_name);
}

int
main(void)
{
    test_eval_sizeof_int();
    test_eval_sizeof_struct();
    test_max_two();
    test_min_two();
    test_max_three();
    test_strlen();
    test_strcmp_equal();
    test_strcmp_less();
    test_paste_str_int();
    test_paste_str_str();
    return 0;
}
