#include "core/buffer.h"
#include "core/hash.h"
#include "core/type_info.h"

#include <string.h>

const n00b_buffer_t *literal_state = b"wp006-buffer";
const n00b_buffer_t *call_state    = b"call";
const n00b_buffer_t *raw_state     = b"raw";
const n00b_buffer_t *hex_state     = b"hex";
const n00b_buffer_t *zero_state    = b"\0\0\0\0\0";

static int
check_buffer(const n00b_buffer_t *state,
             const char *expected,
             size_t expected_len,
             size_t expected_alloc_len)
{
    if (state == nullptr || state->data == nullptr) {
        return 10;
    }
    if (state->byte_len != expected_len) {
        return 11;
    }
    if (state->alloc_len != expected_alloc_len) {
        return 17;
    }
    if (expected_len != 0 && memcmp(state->data, expected, expected_len) != 0) {
        return 12;
    }
    if (state->lock != nullptr || state->allocator != nullptr) {
        return 13;
    }
    if (state->flags != N00B_BUF_F_BORROWED) {
        return 14;
    }
    if (state->scan_kind != N00B_GC_SCAN_KIND_NONE
        || state->scan_cb != nullptr || state->scan_user != nullptr) {
        return 15;
    }
    if (n00b_obj_typehash((void *)state->data) != typehash(char *)) {
        return 18;
    }

    n00b_uint128_t generic_hash = n00b_hash((void *)state, nullptr);
    n00b_uint128_t buffer_hash  = n00b_buffer_hash((n00b_buffer_t *)state);
    if (generic_hash != buffer_hash) {
        return 16;
    }

    return 0;
}

int
main(void)
{
    int rc = check_buffer(literal_state, "wp006-buffer", 12, 16);
    if (rc != 0) {
        return rc;
    }
    rc = check_buffer(call_state, "call", 4, 4);
    if (rc != 0) {
        return 20 + rc;
    }
    rc = check_buffer(raw_state, "raw", 3, 4);
    if (rc != 0) {
        return 40 + rc;
    }
    rc = check_buffer(hex_state, "hex", 3, 4);
    if (rc != 0) {
        return 60 + rc;
    }
    rc = check_buffer(zero_state, "\0\0\0\0\0", 5, 8);
    if (rc != 0) {
        return 80 + rc;
    }

    return 0;
}
