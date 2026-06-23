#include "n00b.h"
#include "adt/dict.h"

#include <stdint.h>

n00b_dict_t(uint64_t, uint64_t) dict = d{42: 4200};

int
main(void)
{
    bool     found = false;
    uint64_t key   = 42;
    uint64_t value = n00b_dict_get(&dict, key, &found);
    return found && value == 4200 ? 0 : 1;
}
