// WP-011 Phase 2: block-scope mutable dict literal target.  The
// lifetime check fires before the Phase 2 stub-lowering error, mirror-
// ing the WP-010 list literal block-scope mutable diagnostic (see
// err_list_literal_local_mutable.c).

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

void
bad_local(void)
{
    n00b_dict_t(int, int) values = d{1: 2, 3: 4};
    (void)values;
}
