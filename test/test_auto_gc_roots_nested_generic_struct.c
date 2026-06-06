// test_auto_gc_roots_nested_generic_struct.c
//
// Regression test for static aggregates that embed a generic-struct
// field. The auto-root pass must run after `_generic_struct` lowering
// has emitted the concrete struct definition and rewritten this member
// to `struct <generated-tag>`, otherwise the nested pointer-bearing
// fields are invisible and no root table entry is emitted.
//
// Expected emit includes at least:
//     { .addr = (void *) & (g_endpoint).events.data, .num_words = 1 },

typedef unsigned long size_t;

typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;

typedef enum n00b_gc_scan_kind_t {
    N00B_GC_SCAN_NONE = 0,
} n00b_gc_scan_kind_t;

typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);

#define n00b_list_tid(T) typeid("n00b_list", T)
#define n00b_list_t(T)                                                      \
    _generic_struct n00b_list_tid(T) {                                      \
        T                  *data;                                           \
        size_t              len;                                            \
        size_t              cap;                                            \
        n00b_rwlock_t      *lock;                                           \
        n00b_allocator_t   *allocator;                                      \
        n00b_gc_scan_kind_t scan_kind;                                      \
        n00b_gc_scan_cb_t   scan_cb;                                        \
        void               *scan_user;                                      \
    }

typedef struct n00b_string_t n00b_string_t;

static struct endpoint_like_t {
    int marker;
    n00b_list_t(n00b_string_t *) events;
} g_endpoint;

int
main(void)
{
    return (g_endpoint.marker == 0
            && g_endpoint.events.data == ((void *)0))
               ? 0
               : 1;
}
