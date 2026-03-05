#pragma once

#include <assert.h>
#include "ncc.h"
#include "lib/macros.h"

// Pointer types need typedef aliases for token-paste to work.
typedef void       *ncc_voidptr_t;
typedef char       *ncc_charptr_t;
typedef const char *ncc_const_charptr_t;

#define ncc_option_tid(T) typeid(ncc_option, T)
#define ncc_option_t(T)   struct ncc_option_tid(T)

#define ncc_option_decl(T)                       \
    struct ncc_option_tid(T) {                    \
        bool has_value;                            \
        T    value;                                \
    }

#define ncc_option_set(T, x)                     \
    ((ncc_option_t(T)){                           \
        .has_value = true,                         \
        .value     = (x),                          \
    })

#define ncc_option_none(T)                        \
    ((ncc_option_t(T)){                           \
        .has_value = false,                        \
        .value     = {},                           \
    })

#define ncc_option_get(x)                         \
    ({                                             \
        auto _bl_o = (x);                          \
        assert(_bl_o.has_value);                   \
        _bl_o.value;                               \
    })

#define ncc_option_is_set(x) ((x).has_value)

#define ncc_option_get_or_else(x, y)              \
    ({                                             \
        auto _bl_o = (x);                          \
        _bl_o.has_value ? _bl_o.value : (y);       \
    })

#define ncc_option_match(x, set, none) ((x).has_value ? (set) : (none))

#define ncc_option_from_nullable(T, ptr)          \
    ({                                             \
        ncc_option_t(T) _opt;                     \
        auto _p = (ptr);                           \
        if (_p) {                                  \
            _opt = (ncc_option_t(T)){.has_value = true, .value = _p}; \
        } else {                                   \
            _opt = (ncc_option_t(T)){.has_value = false}; \
        }                                          \
        _opt;                                      \
    })

// Common option types.
ncc_option_decl(int);
ncc_option_decl(int32_t);
ncc_option_decl(uint32_t);
ncc_option_decl(int64_t);
ncc_option_decl(size_t);
ncc_option_decl(ncc_codepoint_t);
ncc_option_decl(ncc_voidptr_t);
ncc_option_decl(ncc_charptr_t);
ncc_option_decl(ncc_const_charptr_t);
