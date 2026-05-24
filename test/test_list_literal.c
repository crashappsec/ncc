#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ncc_runtime.h"

typedef struct n00b_rwlock_t {
    int placeholder;
} n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);

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

typedef n00b_list_t(int) int_list_t;
typedef n00b_list_t(ncc_string_t *) string_list_t;
typedef n00b_list_t(int_list_t) nested_int_list_t;

typedef struct n00b_buffer_t {
    char                *data;
    size_t               byte_len;
    size_t               alloc_len;
    n00b_rwlock_t       *lock;
    n00b_allocator_t    *allocator;
    uint32_t             flags;
    n00b_gc_scan_kind_t  scan_kind;
    n00b_gc_scan_cb_t    scan_cb;
    void                *scan_user;
} n00b_buffer_t;
typedef const n00b_buffer_t *n00b_buffer_ptr_t;
typedef n00b_list_t(n00b_buffer_ptr_t) buffer_list_t;

n00b_list_t(int) module_values = l{1, 2, 3};
int_list_t *module_ptr = l{4, 5};
n00b_list_t(int) *module_direct_ptr = l{13, 14};
string_list_t module_words = l{r"alpha", r"beta"};
nested_int_list_t module_nested = l{l{10, 11}, l{12}};
buffer_list_t module_buffers = l{b"hi", b"\x00z"};

static void
test_module_list(void)
{
    assert(module_values.len == 3);
    assert(module_values.cap == 16);
    assert(module_values.lock != nullptr);
    assert(module_values.data[0] == 1);
    assert(module_values.data[1] == 2);
    assert(module_values.data[2] == 3);

    assert(module_ptr != nullptr);
    assert(module_ptr->len == 2);
    assert(module_ptr->cap == 16);
    assert(module_ptr->lock != nullptr);
    assert(module_ptr->data[0] == 4);
    assert(module_ptr->data[1] == 5);

    assert(module_direct_ptr != nullptr);
    assert(module_direct_ptr->len == 2);
    assert(module_direct_ptr->cap == 16);
    assert(module_direct_ptr->lock != nullptr);
    assert(module_direct_ptr->data[0] == 13);
    assert(module_direct_ptr->data[1] == 14);

    assert(module_words.len == 2);
    assert(module_words.cap == 16);
    assert(module_words.data[0]->u8_bytes == 5);
    assert(memcmp(module_words.data[0]->data, "alpha", 5) == 0);
    assert(module_words.data[1]->u8_bytes == 4);
    assert(memcmp(module_words.data[1]->data, "beta", 4) == 0);

    assert(module_nested.len == 2);
    assert(module_nested.cap == 16);
    assert(module_nested.data[0].len == 2);
    assert(module_nested.data[0].cap == 16);
    assert(module_nested.data[0].data[0] == 10);
    assert(module_nested.data[0].data[1] == 11);
    assert(module_nested.data[1].len == 1);
    assert(module_nested.data[1].data[0] == 12);

    assert(module_buffers.len == 2);
    assert(module_buffers.cap == 16);
    assert(module_buffers.data[0]->byte_len == 2);
    assert(memcmp(module_buffers.data[0]->data, "hi", 2) == 0);
    assert(module_buffers.data[1]->byte_len == 2);
    assert(module_buffers.data[1]->data[0] == '\0');
    assert(module_buffers.data[1]->data[1] == 'z');
}

static void
test_local_const_list(void)
{
    const n00b_list_t(int) local = l{6, 7, 8};

    assert(local.len == 3);
    assert(local.cap == 16);
    assert(local.lock != nullptr);
    assert(local.data[0] == 6);
    assert(local.data[1] == 7);
    assert(local.data[2] == 8);
}

int
main(void)
{
    test_module_list();
    test_local_const_list();
    return 0;
}
