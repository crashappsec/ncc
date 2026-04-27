#include <stdio.h>

int
main(void)
{
    static const unsigned char output[] = {'A', '\0', 'B'};

    return fwrite(output, 1, sizeof(output), stdout) == sizeof(output) ? 0 : 1;
}
