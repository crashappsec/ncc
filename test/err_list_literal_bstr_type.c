#include <stddef.h>
#include <stdint.h>

typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);
typedef enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT = 0,
} n00b_gc_scan_kind_t;

#define n00b_list_tid(T) typeid("n00b_list", T)
#define n00b_list_t(T)                                                                \
    _generic_struct n00b_list_tid(T) {                                                \
        T                  *data;                                                     \
        size_t              len;                                                      \
        size_t              cap;                                                      \
        n00b_rwlock_t      *lock;                                                     \
        n00b_allocator_t   *allocator;                                                \
        n00b_gc_scan_kind_t scan_kind;                                                \
        n00b_gc_scan_cb_t   scan_cb;                                                  \
        void               *scan_user;                                                \
    }

n00b_list_t(int) bad_buffers = l{b"not an int"};
