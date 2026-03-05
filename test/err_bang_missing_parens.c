// Broken bang: missing parens on function call before !
// Should error: postfix ! needs a function call producing a result type.
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

int_result_t use_value(void)
{
    // Bug: forgot the () on get_value — this is just an identifier with !
    int x = get_value!;
    return (int_result_t){.is_ok = true, .ok = x, .err = 0};
}

int main(void) { return 0; }
