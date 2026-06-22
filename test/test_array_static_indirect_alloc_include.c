#include "n00b.h"
#include "core/runtime.h"

n00b_array_t(int) state = [5, 8, 13];

int
main(void)
{
    if (state.len != 3 || state.cap != 3 || state.data == nullptr) {
        return 10;
    }
    if (state.data[0] != 5 || state.data[1] != 8 || state.data[2] != 13) {
        return 11;
    }
    return 0;
}
