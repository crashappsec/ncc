#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define N00B_STATIC_IMAGE_CONTRACT_VERSION 1u
#define N00B_STATIC_IMAGE_ENDIAN_LITTLE 1u
#define N00B_STATIC_IMAGE_HOST_ENDIAN N00B_STATIC_IMAGE_ENDIAN_LITTLE
#define N00B_STATIC_IMAGE_ABI_INIT                   \
    {                                                \
        .version       = 1u,                         \
        .pointer_bytes = (uint8_t)sizeof(void *),    \
        .size_t_bytes  = (uint8_t)sizeof(size_t),    \
        .char_bits     = 8,                          \
        .endian        = N00B_STATIC_IMAGE_ENDIAN_LITTLE, \
    }

typedef enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT  = 0,
    N00B_GC_SCAN_KIND_NONE     = 1,
    N00B_GC_SCAN_KIND_ALL      = 2,
    N00B_GC_SCAN_KIND_CALLBACK = 4,
} n00b_gc_scan_kind_t;

typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);

typedef struct {
    uint64_t        stride;
    uint64_t        count;
    uint64_t        offset_count;
    const uint64_t *offsets;
} n00b_gc_struct_layout_t;

typedef struct {
    const void        *start;
    uint64_t           len;
    uint64_t           tinfo;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t  scan_cb;
    void              *scan_user;
    uint64_t           object_id;
    const char        *file;
    uint32_t           flags;
} n00b_static_object_desc_t;

typedef enum n00b_static_image_payload_kind_t : uint8_t {
    N00B_STATIC_IMAGE_PAYLOAD_NONE  = 0,
    N00B_STATIC_IMAGE_PAYLOAD_BYTES = 1,
} n00b_static_image_payload_kind_t;

typedef struct n00b_static_image_abi_t {
    uint32_t version;
    uint8_t  pointer_bytes;
    uint8_t  size_t_bytes;
    uint8_t  char_bits;
    uint8_t  endian;
} n00b_static_image_abi_t;

typedef struct n00b_static_image_request_t {
    uint32_t                         version;
    uint64_t                         type_hash;
    n00b_static_image_payload_kind_t payload_kind;
    const void                      *payload;
    uint64_t                         payload_len;
    n00b_static_image_abi_t          target_abi;
    uint32_t                         object_flags;
    n00b_gc_scan_kind_t              required_scan_kind;
} n00b_static_image_request_t;

typedef struct n00b_static_image_dependency_t {
    const n00b_static_object_desc_t *desc;
    uint64_t                         relocation_offset;
    const char                      *role;
} n00b_static_image_dependency_t;

typedef struct n00b_static_image_response_t {
    uint32_t                              version;
    const n00b_static_image_request_t    *request;
    const void                           *object_start;
    uint64_t                              object_len;
    n00b_gc_scan_kind_t                   scan_kind;
    n00b_gc_scan_cb_t                     scan_cb;
    void                                 *scan_user;
    const n00b_static_image_dependency_t *dependencies;
    uint64_t                              dependency_count;
} n00b_static_image_response_t;

void
n00b_gc_scan_cb_struct_layout(n00b_gc_map_t *m, void *user)
{
    (void)m;
    (void)user;
}

#define N00B_BUF_F_BORROWED (1 << 1)

typedef struct n00b_buffer_t {
    char               *data;
    size_t              byte_len;
    size_t              alloc_len;
    void               *lock;
    void               *allocator;
    int32_t             flags;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t   scan_cb;
    void               *scan_user;
} n00b_buffer_t;

typedef struct {
    uint64_t             magic;
    uint64_t             byte_len;
    const unsigned char *bytes;
    uint64_t             constructor_cookie;
} n00b_static_image_test_t;

const n00b_static_image_test_t *readonly_image = ncc_static_image("static");
n00b_static_image_test_t       *mutable_image  = ncc_static_image("mutable");
const n00b_static_image_test_t *named_image    = ncc_static_image(.raw = "named");
const n00b_buffer_t            *buffer_literal = b"buffer";

int
main(void)
{
    assert(readonly_image->magic == 0x4E30304253494D47ULL);
    assert(readonly_image->byte_len == 6);
    assert(memcmp(readonly_image->bytes, "static", 6) == 0);
    assert(readonly_image->constructor_cookie == 0);

    assert(mutable_image->magic == 0x4E30304253494D47ULL);
    assert(mutable_image->byte_len == 7);
    assert(memcmp(mutable_image->bytes, "mutable", 7) == 0);
    mutable_image->constructor_cookie = 99;
    assert(mutable_image->constructor_cookie == 99);

    assert(named_image->magic == 0x4E30304253494D47ULL);
    assert(named_image->byte_len == 5);
    assert(memcmp(named_image->bytes, "named", 5) == 0);
    assert(named_image->constructor_cookie == 0);

    assert(buffer_literal->byte_len == 6);
    assert(buffer_literal->alloc_len == 8);
    assert(memcmp(buffer_literal->data, "buffer", 6) == 0);
    assert(buffer_literal->lock == 0);
    assert(buffer_literal->allocator == 0);
    assert(buffer_literal->flags == N00B_BUF_F_BORROWED);
    assert(buffer_literal->scan_kind == N00B_GC_SCAN_KIND_NONE);
    assert(buffer_literal->scan_cb == 0);
    assert(buffer_literal->scan_user == 0);

    return 0;
}
