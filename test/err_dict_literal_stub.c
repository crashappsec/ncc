// WP-011 Phase 3c.ii.a partial-stub fixture; DELETE when Phase
// 3c.ii.b implements buffer-key lowering.  Phase 3c.ii.a shipped
// r-string-key support, so this fixture switched its trigger to a
// b-buffer key (`d{b"abc": 1}`); buffer keys remain routed to the
// partial-stub diagnostic until 3c.ii.b lands.

#include <stddef.h>
#include <stdint.h>

#include "ncc_runtime.h"

typedef struct n00b_buffer_t n00b_buffer_t;
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

// Buffer-keyed dict literal triggers the Phase 3c.ii.a partial-stub.
n00b_dict_t(n00b_buffer_t *, int) x = d{b"abc": 1};
