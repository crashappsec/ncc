// test_gc_typemap.c -- WP-020 link-time type->GC-map emission fixture.
//
// Meson runs this source through `ncc -E` and asserts on the transformed output:
// direct data pointers are emitted as offsetof entries, direct function
// pointers are omitted, conservative skip cases emit no descriptor, and fallback
// generated spellings use C23 attributes/assertions.

#include <stdint.h>

typedef struct gcmap_payload_t {
    uint64_t value;
} gcmap_payload_t;

typedef struct gcmap_direct_fn_t {
    void             (*callback)(void);
    gcmap_payload_t  *payload;
} gcmap_direct_fn_t;

typedef void (*gcmap_callback_t)(void);

typedef struct gcmap_typedef_fn_t {
    gcmap_callback_t  callback;
    gcmap_payload_t  *payload;
} gcmap_typedef_fn_t;

typedef struct gcmap_user_scan_field_t {
    void             *scan_user;
    gcmap_payload_t  *payload;
} gcmap_user_scan_field_t;

typedef unsigned char n00b_gc_scan_kind_t;
typedef void (*n00b_gc_scan_cb_t)(void *, void *);
typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;

typedef struct gcmap_runtime_shape_t {
    n00b_gc_scan_kind_t  scan_kind;
    n00b_gc_scan_cb_t    scan_cb;
    void                *scan_user;
    n00b_rwlock_t       *lock;
    n00b_allocator_t    *allocator;
    gcmap_payload_t     *payload;
} gcmap_runtime_shape_t;

typedef unsigned char gcmap_scan_kind_t;

typedef struct gcmap_runtime_near_miss_t {
    gcmap_scan_kind_t  scan_kind;
    void              *scan_cb;
    void              *scan_user;
    gcmap_payload_t   *payload;
} gcmap_runtime_near_miss_t;

typedef struct gcmap_pointer_array_t {
    gcmap_payload_t *items[2];
} gcmap_pointer_array_t;

typedef union gcmap_union_mixed_t {
    void            (*callback)(void);
    gcmap_payload_t *payload;
} gcmap_union_mixed_t;

static uint64_t h_direct = typehash(gcmap_direct_fn_t *);
static uint64_t h_typedef = typehash(gcmap_typedef_fn_t *);
static uint64_t h_user    = typehash(gcmap_user_scan_field_t *);
static uint64_t h_runtime = typehash(gcmap_runtime_shape_t *);
static uint64_t h_near    = typehash(gcmap_runtime_near_miss_t *);
static uint64_t h_array   = typehash(gcmap_pointer_array_t *);
static uint64_t h_union   = typehash(gcmap_union_mixed_t *);

int
main(void)
{
    return (int)((h_direct ^ h_typedef ^ h_user ^ h_runtime ^ h_near
                  ^ h_array ^ h_union)
                 == 0);
}
