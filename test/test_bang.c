#include <stdio.h>

typedef struct {
    bool is_ok;
    int  ok;
    int  err;
} int_result_t;

int_result_t get_value(void)
{
    return (int_result_t){.is_ok = true, .ok = 42, .err = 0};
}

int_result_t get_error(void)
{
    return (int_result_t){.is_ok = false, .ok = 0, .err = -1};
}

int_result_t use_value(void)
{
    int x = get_value()!;
    return (int_result_t){.is_ok = true, .ok = x + 1, .err = 0};
}

int_result_t use_multiple(void)
{
    int a = get_value()!;
    int b = get_value()!;
    return (int_result_t){.is_ok = true, .ok = a + b, .err = 0};
}

int_result_t propagate_error(void)
{
    int x = get_error()!;
    return (int_result_t){.is_ok = true, .ok = x, .err = 0};
}

int
main(void)
{
    int_result_t r1 = use_value();
    if (!r1.is_ok || r1.ok != 43) {
        fprintf(stderr, "FAIL: use_value: is_ok=%d ok=%d\n", r1.is_ok, r1.ok);
        return 1;
    }
    printf("PASS: use_value() = %d\n", r1.ok);

    int_result_t r2 = use_multiple();
    if (!r2.is_ok || r2.ok != 84) {
        fprintf(stderr, "FAIL: use_multiple: is_ok=%d ok=%d\n",
                r2.is_ok, r2.ok);
        return 1;
    }
    printf("PASS: use_multiple() = %d\n", r2.ok);

    int_result_t r3 = propagate_error();
    if (r3.is_ok) {
        fprintf(stderr, "FAIL: propagate_error should have failed\n");
        return 1;
    }
    if (r3.err != -1) {
        fprintf(stderr, "FAIL: propagate_error err=%d, expected -1\n", r3.err);
        return 1;
    }
    printf("PASS: propagate_error() => err=%d\n", r3.err);

    printf("All bang tests passed!\n");
    return 0;
}
