// Broken constexpr: expression references undefined type.
#include <stdio.h>

int main(void)
{
    // no_such_type doesn't exist — constexpr compile should fail
    int x = constexpr_eval(sizeof(no_such_type));
    printf("%d\n", x);
    return 0;
}
