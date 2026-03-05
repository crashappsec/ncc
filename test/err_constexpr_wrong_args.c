// Broken constexpr: wrong number of arguments.
#include <stdio.h>

int main(void)
{
    // constexpr_eval takes exactly 1 argument
    int x = constexpr_eval(sizeof(int), sizeof(long));
    printf("%d\n", x);
    return 0;
}
