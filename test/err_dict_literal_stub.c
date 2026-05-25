// WP-011 Phase 3c.ii.b partial-stub fixture; DELETE when a future
// phase widens the pointer-key surface to other registered pointer
// types (typedef'd struct pointers, generic struct pointers, etc.).
// Phase 3c.ii.a shipped r-string-key support; Phase 3c.ii.b adds
// buffer-key support; this fixture pins the partial-stub diagnostic
// for the residual pointer-key kinds that still route to the stub.
//
// Trigger: a generic struct-pointer key (`struct foo *`) that is
// neither `n00b_string_t *` nor `n00b_buffer_t *`.  ncc classifies
// it as `DICT_KEY_KIND_POINTER` (the wildcard-pointer kind) and
// falls through to `lower_dict_literal_pointer_key_stub`.

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
