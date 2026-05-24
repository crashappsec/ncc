// WP-011 Phase 3c.i: positive scalar-keyed dict literal coverage.
//
// Compiles and runs a program containing several scalar-keyed dict
// literal forms (small dict, large dict that forces pow2 > 16,
// nested-list values, typedef-aliased dict target, pointer-target
// dict, empty dict), then walks the runtime layout via the bucket
// table and asserts each lookup returns the helper-emitted value.
// Pointer-keyed dicts route to the Phase 3c.i partial-stub diagnostic
// — see err_dict_literal_stub.c for that fixture.
//
// The fixture mirrors test_list_literal.c's compile-and-run shape:
// every libn00b type the ncc xform pass + the static-init helper
// reach for is shadowed here so the binary links standalone (no
// libn00b dependency).  The dict / store / bucket struct shapes
// MUST stay in sync with the helper's emitted designated
// initializer; see n00b_static-init-helper.c::emit_dict_image.

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ncc_runtime.h"

// Mirrors of the libn00b dict/store/bucket ABI surface, sized so
// pointer-arithmetic in this fixture matches the helper's layout.
typedef struct n00b_rwlock_t {
    int placeholder;
} n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);
typedef unsigned _BitInt(128) n00b_uint128_t;
typedef _Atomic(uint32_t) n00b_futex_t;

typedef enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT  = 0,
    N00B_GC_SCAN_KIND_NONE     = 1,
    N00B_GC_SCAN_KIND_ALL      = 2,
    N00B_GC_SCAN_KIND_CALLBACK = 4,
} n00b_gc_scan_kind_t;

typedef enum n00b_static_identity_kind_t : uint8_t {
    N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_OBJECT  = 3,
    N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_PAYLOAD = 4,
} n00b_static_identity_kind_t;

typedef struct n00b_static_identity_t {
    uint32_t                    version;
    n00b_static_identity_kind_t kind;
    uint8_t                     reserved[3];
    const char                 *namespace_id;
    const char                 *object_key;
} n00b_static_identity_t;

typedef struct n00b_static_object_desc_t {
    const void                    *start;
    uint64_t                       len;
    uint64_t                       tinfo;
    n00b_gc_scan_kind_t            scan_kind;
    n00b_gc_scan_cb_t              scan_cb;
    void                          *scan_user;
    uint64_t                       object_id;
    const char                    *file;
    const n00b_static_identity_t  *identity;
    uint32_t                       flags;
} n00b_static_object_desc_t;

typedef enum n00b_static_image_payload_kind_t : uint8_t {
    N00B_STATIC_IMAGE_PAYLOAD_NONE = 0,
    N00B_STATIC_IMAGE_PAYLOAD_BYTES = 1,
} n00b_static_image_payload_kind_t;

typedef struct n00b_static_image_abi_t {
    uint32_t version;
    uint8_t  pointer_bytes;
    uint8_t  size_t_bytes;
    uint8_t  char_bits;
    uint8_t  endian;
} n00b_static_image_abi_t;

typedef struct n00b_static_init_arg_t {
    const char *name;
} n00b_static_init_arg_t;

typedef struct n00b_static_image_request_t {
    uint32_t                         version;
    uint64_t                         type_hash;
    n00b_static_image_payload_kind_t payload_kind;
    const void                      *payload;
    uint64_t                         payload_len;
    const n00b_static_init_arg_t    *args;
    uint64_t                         arg_count;
    n00b_static_image_abi_t          target_abi;
    uint32_t                         object_flags;
    n00b_gc_scan_kind_t              required_scan_kind;
    const char                      *identity_namespace;
    const char                      *identity_object_key;
    const char                      *identity_payload_key;
} n00b_static_image_request_t;

typedef struct n00b_static_image_dependency_t {
    const n00b_static_object_desc_t *desc;
    uint64_t                         relocation_offset;
    const char                      *role;
} n00b_static_image_dependency_t;

typedef struct n00b_static_image_response_t {
    uint32_t                                version;
    const n00b_static_image_request_t      *request;
    const void                             *object_start;
    uint64_t                                object_len;
    n00b_gc_scan_kind_t                     scan_kind;
    n00b_gc_scan_cb_t                       scan_cb;
    void                                   *scan_user;
    const n00b_static_image_dependency_t   *dependencies;
    uint64_t                                dependency_count;
} n00b_static_image_response_t;

typedef struct {
    uint64_t stride;
    uint64_t count;
    uint64_t offset_count;
    const uint64_t *offsets;
} n00b_gc_struct_layout_t;

typedef struct {
    uint64_t stride;
    uint64_t offset;
    uint64_t count;
} n00b_gc_struct_array_t;

enum n00b_static_object_flags_t : uint32_t {
    N00B_STATIC_OBJECT_F_READONLY = 1u << 0,
    N00B_STATIC_OBJECT_F_MUTABLE  = 1u << 1,
};

void
n00b_gc_scan_cb_struct_layout(n00b_gc_map_t *m, void *user)
{
    (void)m;
    (void)user;
}

void
n00b_gc_scan_cb_struct_field(n00b_gc_map_t *m, void *user)
{
    (void)m;
    (void)user;
}

// Dict bucket / store / dict struct layouts MUST match
// n00b/include/adt/dict.h.  See WP-011 Phase 3b.fix decisions D-070
// (lock model) and D-072 (`_migration_state` rename).
typedef struct n00b_dict_bucket_t {
    n00b_uint128_t   hv;
    uint32_t         insert_order;
    _Atomic uint32_t flags;
} n00b_dict_bucket_t;

typedef struct __n00b_internal_type_erased_store_t {
    uint32_t            last_slot;
    uint32_t            threshold;
    _Atomic uint32_t    used_count;
    n00b_dict_bucket_t *buckets;
    void              **keys;
    void              **values;
} __n00b_internal_type_erased_store_t;

typedef n00b_uint128_t (*n00b_hash_fn)(void *);

#define n00b_dict_store_tid(K, V) typeid("store", K, V)
#define n00b_dict_store_t(K, V)                                               \
    _generic_struct n00b_dict_store_tid(K, V) {                               \
        uint32_t            last_slot;                                        \
        uint32_t            threshold;                                        \
        _Atomic uint32_t    used_count;                                       \
        n00b_dict_bucket_t *buckets;                                          \
        K                  *keys;                                             \
        V                  *values;                                           \
    }

// The struct layout MUST match what the static-init helper emits as
// a designated initializer (see emit_dict_image in
// n00b-static-init-helper.c).  Field order isn't load-bearing for the
// designated initializer itself, but the field NAMES are.
#define n00b_dict_tid(K, V) typeid("n00b_dict", K, V)
#define n00b_dict_t(K, V)                                                     \
    _generic_struct n00b_dict_tid(K, V) {                                     \
        _Atomic(n00b_dict_store_t(K, V) *) store;                             \
        n00b_hash_fn         fn;                                              \
        n00b_allocator_t    *allocator;                                       \
        _Atomic uint32_t     insertion_epoch;                                 \
        _Atomic int64_t      wait_ct;                                         \
        _Atomic int64_t      length;                                          \
        n00b_futex_t         _migration_state;                                \
        n00b_rwlock_t       *lock;                                            \
        uint8_t              cache         : 1;                               \
        uint8_t              skip_obj_hash : 1;                               \
        n00b_gc_scan_kind_t  scan_kind;                                       \
        n00b_gc_scan_cb_t    scan_cb;                                         \
        void                *scan_user;                                       \
    }

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

// ----------------------------------------------------------------------------
// Module-scope dict literal coverage matrix.
// ----------------------------------------------------------------------------

// Small dict: capacity stays at the floor (16).
n00b_dict_t(int, int) small_dict = d{1: 10, 2: 20, 3: 30};

// Larger dict: 20 entries forces pow2_ceil(20) == 32.
n00b_dict_t(int, int) large_dict = d{
    1: 100, 2: 101, 3: 102, 4: 103, 5: 104,
    6: 105, 7: 106, 8: 107, 9: 108, 10: 109,
    11: 110, 12: 111, 13: 112, 14: 113, 15: 114,
    16: 115, 17: 116, 18: 117, 19: 118, 20: 119,
};

// Nested list value (D-069 covers nested supported containers as
// dict values).  We typedef the list parameterization first so the
// dict's value type resolves through ncc's list_types table; the
// dict's typeid-based K/V extraction carries the typedef name
// through to the helper.
typedef n00b_list_t(int) int_list_t;
n00b_dict_t(int, int_list_t) nested_dict = d{1: l{2, 3}, 4: l{5, 6}};

// Typedef-aliased dict target.
typedef n00b_dict_t(int, int) my_dict_t;
my_dict_t aliased_dict = d{7: 70, 8: 80};

// Pointer-target dict (D-067 — pointer-target shape).
n00b_dict_t(int, int) *ptr_dict = d{42: 4200};

// Empty dict (D-064 — empty `d{}` is a valid empty dict literal).
n00b_dict_t(int, int) empty_dict = d{};

// ----------------------------------------------------------------------------
// Tiny dict lookup that walks the helper-emitted buckets/keys/values.
// Mirrors `compute_hash` + the runtime probe loop for scalar keys
// (`skip_obj_hash=true`, `fn=nullptr`, key compared by sizeof-key
// memcmp).  We avoid pulling in libn00b: the fixture's purpose is
// to validate the helper's encoded layout is queryable end-to-end.
// ----------------------------------------------------------------------------

// XXH3 isn't directly available to test code; instead we walk the
// bucket table linearly and match by the stored key bytes, which is
// sufficient to confirm the helper put each (key, value) pair where
// a lookup using the runtime's hash would land them.  The runtime
// dict lookup also reads bucket->hv, but for this round-trip check
// the key match alone is the load-bearing assertion.
//
// `K` and `V` must be word-sized scalars (int here), which matches
// the helper's `void **keys` / `void **values` materialization.
static int
int_dict_lookup(__n00b_internal_type_erased_store_t *store, int key,
                int *out)
{
    uint32_t cap = store->last_slot + 1u;
    int     *keys   = (int *)store->keys;
    int     *values = (int *)store->values;
    for (uint32_t s = 0; s < cap; s++) {
        if (store->buckets[s].hv == (n00b_uint128_t)0) {
            continue;
        }
        if (keys[s] == key) {
            *out = values[s];
            return 1;
        }
    }
    return 0;
}

static int
nested_dict_lookup(__n00b_internal_type_erased_store_t *store, int key,
                   int_list_t *out)
{
    uint32_t    cap   = store->last_slot + 1u;
    int        *keys  = (int *)store->keys;
    int_list_t *values = (int_list_t *)store->values;
    for (uint32_t s = 0; s < cap; s++) {
        if (store->buckets[s].hv == (n00b_uint128_t)0) {
            continue;
        }
        if (keys[s] == key) {
            *out = values[s];
            return 1;
        }
    }
    return 0;
}

static int64_t
dict_length(int64_t length)
{
    return atomic_load_explicit((_Atomic int64_t *)&length,
                                memory_order_relaxed);
}

static void
test_small_dict(void)
{
    assert(small_dict.lock == nullptr);
    assert(small_dict._migration_state == 0);
    assert(small_dict.skip_obj_hash == 1);
    assert(atomic_load_explicit(&small_dict.length,
                                memory_order_relaxed)
           == 3);

    auto store = small_dict.store;
    assert(store != nullptr);
    assert(store->last_slot == 15u);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 3u);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    int v;
    assert(int_dict_lookup(erased_store, 1, &v) && v == 10);
    assert(int_dict_lookup(erased_store, 2, &v) && v == 20);
    assert(int_dict_lookup(erased_store, 3, &v) && v == 30);
    assert(!int_dict_lookup(erased_store, 999, &v));
}

static void
test_large_dict(void)
{
    auto store = large_dict.store;
    assert(store != nullptr);
    // 20 entries → capacity must scale past 16; pow2_ceil(20) = 32.
    assert(store->last_slot == 31u);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 20u);
    assert(atomic_load_explicit(&large_dict.length,
                                memory_order_relaxed)
           == 20);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    for (int k = 1; k <= 20; k++) {
        int v = -1;
        assert(int_dict_lookup(erased_store, k, &v));
        assert(v == 99 + k);
    }
    int v;
    assert(!int_dict_lookup(erased_store, 21, &v));
}

static void
test_nested_dict(void)
{
    auto store = nested_dict.store;
    assert(store != nullptr);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 2u);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    int_list_t v;
    assert(nested_dict_lookup(erased_store, 1, &v));
    assert(v.len == 2);
    assert(v.data[0] == 2);
    assert(v.data[1] == 3);

    assert(nested_dict_lookup(erased_store, 4, &v));
    assert(v.len == 2);
    assert(v.data[0] == 5);
    assert(v.data[1] == 6);
}

static void
test_aliased_dict(void)
{
    auto store = aliased_dict.store;
    assert(store != nullptr);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 2u);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    int v;
    assert(int_dict_lookup(erased_store, 7, &v) && v == 70);
    assert(int_dict_lookup(erased_store, 8, &v) && v == 80);
}

static void
test_pointer_target_dict(void)
{
    assert(ptr_dict != nullptr);
    auto store = ptr_dict->store;
    assert(store != nullptr);
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 1u);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    int v;
    assert(int_dict_lookup(erased_store, 42, &v) && v == 4200);
}

static void
test_empty_dict(void)
{
    auto store = empty_dict.store;
    assert(store != nullptr);
    assert(store->last_slot == 15u);  // N00B_DICT_MIN_SIZE - 1.
    assert(atomic_load_explicit(&store->used_count,
                                memory_order_relaxed)
           == 0u);
    assert(atomic_load_explicit(&empty_dict.length,
                                memory_order_relaxed)
           == 0);

    auto erased_store =
        (__n00b_internal_type_erased_store_t *)store;
    int v;
    assert(!int_dict_lookup(erased_store, 0, &v));
}

int
main(void)
{
    test_small_dict();
    test_large_dict();
    test_nested_dict();
    test_aliased_dict();
    test_pointer_target_dict();
    test_empty_dict();
    (void)dict_length;
    return 0;
}
