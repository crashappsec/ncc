#include <stdint.h>

typedef struct {
    uint64_t value;
} unsupported_static_image_t;

const unsupported_static_image_t *bad = ncc_static_image("bad");
