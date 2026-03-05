// Plain C parse error: missing semicolon after struct member.
// Tests that ncc gives a useful parse error for basic C syntax mistakes.
#include <stdio.h>

typedef struct {
    int x;
    int y    // <- missing semicolon
    double z;
} point_t;

int main(void)
{
    point_t p = {1, 2, 3.0};
    printf("%d %d %f\n", p.x, p.y, p.z);
    return 0;
}
