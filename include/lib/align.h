#pragma once

#include <stddef.h>
#include <stdint.h>

static inline size_t
ncc_align_closest_pow2_ceil(size_t v)
{
    if (v <= 1) {
        return 1;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;

    return v;
}
