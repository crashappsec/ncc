// Broken bang: used inside a void function.
// The ! operator needs to return a result type on error.
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

void do_stuff(void)
{
    // Bug: void function can't propagate errors via !
    int x = get_value()!;
    printf("%d\n", x);
}

int main(void) { do_stuff(); return 0; }
