#pragma once

#include <assert.h>
#include "lib/option.h"  // for ncc_voidptr_t typedef
#include "lib/macros.h"

typedef int ncc_err_t;

#define ncc_result_tid(T) typeid(ncc_result, T)
#define ncc_result_t(T)   struct ncc_result_tid(T)

#define ncc_result_decl(T)                        \
    struct ncc_result_tid(T) {                    \
        bool is_ok;                                \
        union {                                    \
            T          ok;                         \
            ncc_err_t err;                        \
        };                                         \
    }

#define ncc_result_ok(T, x) \
    ((ncc_result_t(T)){ .is_ok = true, .ok = (x) })

#define ncc_result_err(T, e) \
    ((ncc_result_t(T)){ .is_ok = false, .err = (e) })

#define ncc_result_is_ok(x)  ((x).is_ok)
#define ncc_result_is_err(x) (!(x).is_ok)

#define ncc_result_get(x)                         \
    ({                                             \
        auto _bl_r = (x);                          \
        assert(_bl_r.is_ok);                       \
        _bl_r.ok;                                  \
    })

#define ncc_result_get_err(x)                     \
    ({                                             \
        auto _bl_r = (x);                          \
        assert(!_bl_r.is_ok);                      \
        _bl_r.err;                                 \
    })

#define ncc_result_get_or_else(x, y)              \
    ({                                             \
        auto _bl_r = (x);                          \
        _bl_r.is_ok ? _bl_r.ok : (y);             \
    })

#define ncc_result_match(x, ok_expr, err_expr) ((x).is_ok ? (ok_expr) : (err_expr))

ncc_result_decl(int);
ncc_result_decl(int64_t);
ncc_result_decl(ncc_voidptr_t);
ncc_result_decl(uint64_t);
ncc_result_decl(bool);
ncc_result_decl(uint8_t);
ncc_result_decl(size_t);
