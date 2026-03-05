// Test header: ncc_result_t using _generic_struct for auto-deduplication.
//
// This replaces ncc_result_decl() — the struct definition is embedded
// in ncc_result_t() itself, and _generic_struct ensures only the first
// occurrence actually defines the struct.

#pragma once

#include <assert.h>

typedef int ncc_err_t;

#define ncc_result_tid(T) typeid("result", T)

#define ncc_result_t(T)                                    \
    _generic_struct ncc_result_tid(T) {                    \
        bool is_ok;                                         \
        union {                                             \
            T          ok;                                  \
            ncc_err_t err;                                 \
        };                                                  \
    }

#define ncc_result_ok(T, x) \
    ((ncc_result_t(T)){ .is_ok = true, .ok = (x) })

#define ncc_result_err(T, e) \
    ((ncc_result_t(T)){ .is_ok = false, .err = (e) })

#define ncc_result_is_ok(x)  ((x).is_ok)
#define ncc_result_is_err(x) (!(x).is_ok)

#define ncc_result_get(x)                     \
    ({                                         \
        auto _bl_r = (x);                      \
        assert(_bl_r.is_ok);                   \
        _bl_r.ok;                              \
    })

#define ncc_result_get_err(x)                 \
    ({                                         \
        auto _bl_r = (x);                      \
        assert(!_bl_r.is_ok);                  \
        _bl_r.err;                             \
    })
