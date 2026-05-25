// WP-011 Phase 3c.iii unsupported-key-type fixture.
//
// `float` is not in ncc's scalar-key whitelist (the runtime's
// `n00b_hash_raw` is sound on raw float bytes, but float equality
// and NaN make `float` a non-key type by design — D-064 forbids
// floating keys at compile time).  The lowering pass classifies it
// as `DICT_KEY_KIND_UNSUPPORTED` and emits a friendly diagnostic
// before any helper is invoked.

#include <stddef.h>
#include <stdint.h>

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

n00b_dict_t(float, int) x = d{1.0f: 1};
