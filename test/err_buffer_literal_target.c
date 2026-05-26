#include <stdint.h>

typedef struct {
    uint64_t value;
} not_a_buffer_t;

const not_a_buffer_t *bad = b"bad";
