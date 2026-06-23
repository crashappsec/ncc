// WP-007 migrated static-init fixture: a file-scope writable dict with
// a generic struct-pointer key (`struct foo *`) is classified as a
// migrated value root, then rejected by the migrated dict guard because
// the no-helper route supports only scalar, r-string, and buffer key
// types. This is distinct from the non-pointer-non-scalar case covered
// by `err_dict_literal_unsupported_key_type.c`.

#include <stddef.h>
#include <stdint.h>

#include "ncc_runtime.h"

typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);
typedef enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT = 0,
} n00b_gc_scan_kind_t;

#define n00b_dict_tid(K, V) typeid("n00b_dict", K, V)
#define n00b_dict_t(K, V)                                                     \
    _generic_struct n00b_dict_tid(K, V) {                                     \
        K                  *keys;                                             \
        V                  *values;                                           \
        size_t              len;                                              \
        size_t              cap;                                              \
        n00b_rwlock_t      *lock;                                             \
        n00b_allocator_t   *allocator;                                        \
        n00b_gc_scan_kind_t scan_kind;                                        \
        n00b_gc_scan_cb_t   scan_cb;                                          \
        void               *scan_user;                                        \
    }

struct foo {
    int x;
};

// The key expression's specifics are not significant: the lowering pass
// rejects the literal before evaluating it, since the key TYPE
// (`struct foo *`) classifies as DICT_KEY_KIND_POINTER.
n00b_dict_t(struct foo *, int) x = d{nullptr: 1};
