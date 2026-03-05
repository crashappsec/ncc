// Broken typeid: missing argument.
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    // typeid requires a type argument
    uint64_t h = typeid();
    printf("%llu\n", (unsigned long long)h);
    return 0;
}
