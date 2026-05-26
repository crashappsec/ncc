#include <stdint.h>

typedef struct {
    uint64_t             magic;
    uint64_t             byte_len;
    const unsigned char *bytes;
    uint64_t             constructor_cookie;
} n00b_static_image_test_t;

const n00b_static_image_test_t *bad = ncc_static_image(123);
