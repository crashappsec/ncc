// WP-011 Phase 3c.iii permanent fixture: a generic struct-pointer
// key (`struct foo *`) is classified as `DICT_KEY_KIND_POINTER`
// (the wildcard-pointer kind — neither `n00b_string_t *` nor
// `n00b_buffer_t *`) and is rejected with the dict pointer-key
// partial-stub diagnostic.  This pins the friendly-named partial-
// stub message for the generic pointer-key path; it is a distinct
// code path from the non-pointer-non-scalar case covered by
// `err_dict_literal_unsupported_key_type.c`.
//
// Renamed from `err_dict_literal_stub.c` in Phase 3c.iii.  When a
// future phase widens the pointer-key surface to additional
// registered pointer types, update or retire this fixture in that
// work-package.

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

// Generic struct-pointer-keyed dict literal triggers the Phase
// 3c.ii.b partial-stub.  The key expression's specifics are not
// significant: the lowering pass rejects the literal before ever
// evaluating it, since the key TYPE (`struct foo *`) classifies
// as DICT_KEY_KIND_POINTER.
n00b_dict_t(struct foo *, int) x = d{nullptr: 1};
