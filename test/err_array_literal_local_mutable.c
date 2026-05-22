#include "lib/array.h"

ncc_array_decl(int);

int
main(void)
{
    ncc_array_t(int) xs = [1, 2, 3];
    return (int)xs.len;
}
