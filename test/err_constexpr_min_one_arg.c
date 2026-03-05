// Broken constexpr_min: needs >= 2 arguments, given only 1.
#include <stdio.h>

int main(void)
{
    int x = constexpr_min(sizeof(int));
    printf("%d\n", x);
    return 0;
}
