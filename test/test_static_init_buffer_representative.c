#include "core/alloc.h"

#include <stdint.h>
#include <string.h>

typedef struct wp005_rep_buffer_t {
    char    *data;
    uint64_t byte_len;
    uint64_t alloc_len;
} wp005_rep_buffer_t;

static wp005_rep_buffer_t *
make_representative_buffer(void)
{
    static const char payload[] = "wp005-buffer";

    wp005_rep_buffer_t *buf = n00b_alloc(wp005_rep_buffer_t);
    buf->byte_len = (uint64_t)(sizeof(payload) - 1);
    buf->alloc_len = 16;
    buf->data = n00b_alloc_array(char, (int64_t)buf->alloc_len);
    memcpy(buf->data, payload, sizeof(payload) - 1);
    return buf;
}

const wp005_rep_buffer_t *state = make_representative_buffer();

int
main(void)
{
    if (state == nullptr || state->data == nullptr) {
        return 10;
    }
    if (state->byte_len != 12 || state->alloc_len != 16) {
        return 11;
    }
    if (memcmp(state->data, "wp005-buffer", 12) != 0) {
        return 12;
    }
    return 0;
}
