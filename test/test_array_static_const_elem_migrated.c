#include "n00b.h"
#include "adt/array.h"

n00b_array_t(const int) state = [1, 2, 3];

int
main(void)
{
    if (state.len != 3 || state.cap != 3 || state.data == nullptr) {
        return 10;
    }
    if (state.data[0] != 1 || state.data[1] != 2 || state.data[2] != 3) {
        return 11;
    }
    return 0;
}
