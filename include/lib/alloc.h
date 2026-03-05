#pragma once

#include "ncc.h"
#include "lib/macros.h"
#include "lib/align.h"

// Core allocation: zero-filled.
#define ncc_alloc(T, ...) \
    ((T *)calloc(1, sizeof(T)))

#define ncc_alloc_array(T, N, ...) \
    ((T *)calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size(n, sz, ...) \
    calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_flex(T1, T2, N2, ...) \
    calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void *
ncc_realloc(void *ptr, size_t sz)
{
    return realloc(ptr, sz);
}

static inline void
ncc_free(void *ptr)
{
    free(ptr);
}
