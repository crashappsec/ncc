// WP-011 Phase 3c.iii missing-helper fixture.
//
// A valid dict literal compiled WITHOUT `--ncc-static-init-helper=PATH`
// must produce the missing-helper diagnostic, paralleling the list /
// array literal missing-helper precedents.  The diagnostic names the
// dict via its friendly `n00b_dict_t(K, V)` form.

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

n00b_dict_t(int, int) x = d{1: 2, 3: 4};
