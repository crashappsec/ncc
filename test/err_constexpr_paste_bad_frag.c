// Broken constexpr_paste: suffix is not an identifier fragment or integer.
#include <stdio.h>

void foo(void)
{
    // "hello world" has a space — not a valid identifier fragment
    int constexpr_paste("item_", "hello world") = 42;
    printf("%d\n", item_hello_world);
}

int main(void) { foo(); return 0; }
