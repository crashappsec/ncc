#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

typedef uint32_t ncc_codepoint_t;
typedef int32_t  ncc_color_t;
typedef unsigned __int128 ncc_uint128_t;
typedef ncc_uint128_t ncc_hash_value_t;
typedef uint64_t ncc_size_t;

// Forward declarations for types used across headers.
typedef struct ncc_string_t     ncc_string_t;
typedef struct ncc_buffer_t     ncc_buffer_t;
typedef struct ncc_dict_t ncc_dict_t;

typedef ncc_hash_value_t (*ncc_hash_fn)(void *);

// Codepoint predicate for scanner match_if.
typedef bool (*ncc_cp_predicate_fn)(ncc_codepoint_t, void *);

#define ncc_min(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a < _b ? _a : _b; })
#define ncc_max(a, b) \
    ({ __typeof__(a) _a = (a), _b = (b); _a > _b ? _a : _b; })

#define ncc_likely(x)   __builtin_expect(!!(x), 1)
#define ncc_unlikely(x) __builtin_expect(!!(x), 0)
