#include "n00b.h"
#include "adt/dict.h"
#include "core/buffer.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/string.h"

#include <stdint.h>
#include <string.h>

n00b_dict_t(uint64_t, uint64_t) state = d{1: 10, 2: 20, 3: 30};
n00b_dict_t(uint64_t, uint64_t) empty = d{};
n00b_dict_t(uint64_t, uint64_t) large = d{
    1: 5,
    8: 16,
    15: 27,
    22: 38,
    29: 49,
    36: 60,
    43: 71,
    50: 82,
    57: 93,
    64: 104,
    71: 115,
    78: 126,
    85: 137,
    92: 148,
    99: 159,
    106: 170,
    113: 181,
    120: 192,
    127: 203,
    134: 214,
};
n00b_dict_t(n00b_string_t *, uint64_t) string_keys = d{
    r"alpha": 101,
    r"bravo": 202,
};
n00b_dict_t(uint64_t, n00b_string_t *) pointer_values = d{
    11: r"eleven",
    12: r"twelve",
};
n00b_dict_t(n00b_buffer_t *, uint64_t) buffer_keys = d{
    b"one": 501,
    b"two": 502,
};
n00b_dict_t(uint64_t, n00b_buffer_t *) buffer_values = d{
    31: b"thirty-one",
    32: b"thirty-two",
};

typedef struct {
    int8_t  font_index;
    int8_t  fg_palette_ix;
    int8_t  bg_palette_ix;
    uint8_t bold;
    uint8_t italic;
} test_style_t;

static const test_style_t style_alpha = {
    .font_index    = -1,
    .fg_palette_ix = -1,
    .bg_palette_ix = -1,
    .bold          = 1,
    .italic        = 0,
};
static const test_style_t style_beta = {
    .font_index    = -1,
    .fg_palette_ix = -1,
    .bg_palette_ix = -1,
    .bold          = 0,
    .italic        = 1,
};

n00b_dict_t(n00b_string_t *, const test_style_t *) style_values = d{
    r"alpha": &style_alpha,
    r"beta": &style_beta,
};

static int
expect_value(n00b_dict_t(uint64_t, uint64_t) *dict,
             uint64_t key, uint64_t expect)
{
    bool     found = false;
    uint64_t value = n00b_dict_get(dict, key, &found);
    return found && value == expect;
}

static int
expect_string_key(n00b_string_t *key, uint64_t expect)
{
    bool     found = false;
    uint64_t value = n00b_dict_get(&string_keys, key, &found);
    return found && value == expect;
}

static int
expect_pointer_value(uint64_t key, uint64_t byte_len)
{
    bool           found = false;
    n00b_string_t *value = n00b_dict_get(&pointer_values, key, &found);

    return found && value != nullptr && value->u8_bytes == byte_len;
}

static int
expect_buffer_key(const char *key, uint64_t expect)
{
    bool           found = false;
    n00b_buffer_t *query =
        n00b_buffer_from_bytes((char *)key, (int64_t)strlen(key));
    uint64_t value = n00b_dict_get(&buffer_keys, query, &found);

    return found && value == expect;
}

static int
expect_buffer_value(uint64_t key, const char *expect)
{
    bool           found = false;
    n00b_buffer_t *value = n00b_dict_get(&buffer_values, key, &found);
    size_t         len   = strlen(expect);

    return found && value != nullptr && value->byte_len == len
        && memcmp(value->data, expect, len) == 0;
}

static int
expect_style_value(n00b_string_t *key, const test_style_t *expect)
{
    bool                found = false;
    const test_style_t *value = n00b_dict_get(&style_values, key, &found);

    if (!found) {
        return 1;
    }
    if (value != expect) {
        return 2;
    }
    if (value->font_index != expect->font_index
        || value->fg_palette_ix != expect->fg_palette_ix
        || value->bg_palette_ix != expect->bg_palette_ix
        || value->bold != expect->bold
        || value->italic != expect->italic) {
        return 3;
    }
    return 0;
}

int
main(void)
{
    if (state.lock != 0 || state.store == nullptr) {
        return 10;
    }
    if (!state.skip_obj_hash) {
        return 11;
    }
    if (!expect_value(&state, 1, 10)
        || !expect_value(&state, 2, 20)
        || !expect_value(&state, 3, 30)) {
        return 12;
    }

    bool     found = true;
    uint64_t miss  = n00b_dict_get(&state, (uint64_t){99}, &found);
    if (found || miss != 0) {
        return 13;
    }

    if (empty.lock != 0 || empty.store == nullptr) {
        return 20;
    }
    found = true;
    (void)n00b_dict_get(&empty, (uint64_t){1}, &found);
    if (found) {
        return 21;
    }

    auto large_store =
        (__n00b_internal_type_erased_store_t *)large.store;
    if (large_store == nullptr || large_store->last_slot != 31u
        || large_store->threshold != 23u || large_store->used_count != 20u) {
        return 40;
    }
    for (uint64_t i = 0; i < 20; i++) {
        uint64_t key = i * 7 + 1;
        uint64_t value = i * 11 + 5;
        if (!expect_value(&large, key, value)) {
            return 41;
        }
    }

    if (string_keys.lock != 0 || string_keys.store == nullptr) {
        return 50;
    }
    if (string_keys.skip_obj_hash
        || string_keys.key_scan_kind != N00B_GC_SCAN_KIND_ALL
        || string_keys.value_scan_kind != N00B_GC_SCAN_KIND_NONE) {
        return 51;
    }
    if (!expect_string_key(r"alpha", 101)
        || !expect_string_key(r"bravo", 202)) {
        return 52;
    }

    auto string_store =
        (__n00b_internal_type_erased_store_t *)string_keys.store;
    if (string_store->used_count != 2u) {
        return 53;
    }
    int string_occupied = 0;
    for (uint32_t i = 0; i <= string_store->last_slot; i++) {
        if (string_store->buckets[i].hv != (n00b_uint128_t)0) {
            string_occupied++;
        }
    }
    if (string_occupied != 2) {
        return 54;
    }

    if (pointer_values.lock != 0 || pointer_values.store == nullptr) {
        return 70;
    }
    if (!pointer_values.skip_obj_hash
        || pointer_values.key_scan_kind != N00B_GC_SCAN_KIND_NONE
        || pointer_values.value_scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 71;
    }
    bool found_ptr = false;
    n00b_string_t *eleven = n00b_dict_get(&pointer_values,
                                          (uint64_t){11},
                                          &found_ptr);
    if (!found_ptr || eleven == nullptr || eleven->u8_bytes != 6) {
        return 72;
    }
    found_ptr = false;
    n00b_string_t *twelve = n00b_dict_get(&pointer_values,
                                          (uint64_t){12},
                                          &found_ptr);
    if (!found_ptr || twelve == nullptr || twelve->u8_bytes != 6) {
        return 73;
    }

    if (buffer_keys.lock != 0 || buffer_keys.store == nullptr) {
        return 74;
    }
    if (buffer_keys.skip_obj_hash
        || buffer_keys.key_scan_kind != N00B_GC_SCAN_KIND_ALL
        || buffer_keys.value_scan_kind != N00B_GC_SCAN_KIND_NONE) {
        return 75;
    }
    if (!expect_buffer_key("one", 501)
        || !expect_buffer_key("two", 502)) {
        return 76;
    }

    if (buffer_values.lock != 0 || buffer_values.store == nullptr) {
        return 77;
    }
    if (!buffer_values.skip_obj_hash
        || buffer_values.key_scan_kind != N00B_GC_SCAN_KIND_NONE
        || buffer_values.value_scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 78;
    }
    if (!expect_buffer_value(31, "thirty-one")
        || !expect_buffer_value(32, "thirty-two")) {
        return 79;
    }

    if (style_values.lock != 0 || style_values.store == nullptr) {
        return 100;
    }
    if (style_values.skip_obj_hash
        || style_values.key_scan_kind != N00B_GC_SCAN_KIND_ALL
        || style_values.value_scan_kind != N00B_GC_SCAN_KIND_NONE) {
        return 101;
    }
    int style_alpha_result = expect_style_value(r"alpha", &style_alpha);
    int style_beta_result  = expect_style_value(r"beta", &style_beta);
    if (style_alpha_result != 0) {
        return 100 + style_alpha_result;
    }
    if (style_beta_result != 0) {
        return 110 + style_beta_result;
    }

    uint64_t state_insert_key   = 4;
    uint64_t state_insert_value = 40;
    uint64_t state_update_key   = 2;
    uint64_t state_update_value = 222;
    n00b_dict_put(&state, state_insert_key, state_insert_value);
    n00b_dict_put(&state, state_update_key, state_update_value);
    if (state.length != 4 || !expect_value(&state, 4, 40)
        || !expect_value(&state, 2, 222)
        || !expect_value(&state, 1, 10)) {
        return 90;
    }

    auto state_store_after =
        (__n00b_internal_type_erased_store_t *)state.store;
    if (state_store_after == nullptr || state_store_after->last_slot != 15u) {
        return 91;
    }

    uint64_t large_insert_key1   = 141;
    uint64_t large_insert_value1 = 225;
    uint64_t large_insert_key2   = 148;
    uint64_t large_insert_value2 = 236;
    uint64_t large_insert_key3   = 155;
    uint64_t large_insert_value3 = 247;
    uint64_t large_insert_key4   = 162;
    uint64_t large_insert_value4 = 258;
    uint64_t large_update_key   = 64;
    uint64_t large_update_value = 1004;
    n00b_dict_put(&large, large_insert_key1, large_insert_value1);
    n00b_dict_put(&large, large_insert_key2, large_insert_value2);
    n00b_dict_put(&large, large_insert_key3, large_insert_value3);
    n00b_dict_put(&large, large_insert_key4, large_insert_value4);
    n00b_dict_put(&large, large_update_key, large_update_value);
    if (large.length != 24 || !expect_value(&large, 141, 225)
        || !expect_value(&large, 148, 236)
        || !expect_value(&large, 155, 247)
        || !expect_value(&large, 162, 258)
        || !expect_value(&large, 64, 1004)
        || !expect_value(&large, 134, 214)) {
        return 92;
    }

    auto large_store_after =
        (__n00b_internal_type_erased_store_t *)large.store;
    if (large_store_after == nullptr) {
        return 93;
    }
    if (large_store_after->last_slot != 63u) {
        return 95;
    }
    if (large_store_after->threshold != 47u) {
        return 96;
    }

    uint64_t ptr_insert_key = 13;
    uint64_t ptr_update_key = 11;
    n00b_string_t *thirteen = r"thirteen";
    n00b_string_t *changed  = r"changed";
    n00b_dict_put(&pointer_values, ptr_insert_key, thirteen);
    n00b_dict_put(&pointer_values, ptr_update_key, changed);
    if (pointer_values.length != 3
        || !expect_pointer_value(13, 8)
        || !expect_pointer_value(11, 7)
        || !expect_pointer_value(12, 6)) {
        return 94;
    }

    eleven = nullptr;
    twelve = nullptr;
    thirteen = nullptr;
    changed  = nullptr;
    n00b_collect(n00b_get_runtime()->default_arena);

    if (!expect_value(&state, 1, 10)
        || !expect_value(&state, 2, 222)
        || !expect_value(&state, 3, 30)) {
        return 30;
    }
    if (!expect_value(&state, 4, 40)) {
        return 31;
    }
    if (!expect_value(&large, 1, 5) || !expect_value(&large, 134, 214)
        || !expect_value(&large, 141, 225)
        || !expect_value(&large, 148, 236)
        || !expect_value(&large, 155, 247)
        || !expect_value(&large, 162, 258)
        || !expect_value(&large, 64, 1004)) {
        return 80;
    }
    if (!expect_string_key(r"alpha", 101)
        || !expect_string_key(r"bravo", 202)) {
        return 81;
    }
    found_ptr = false;
    eleven = n00b_dict_get(&pointer_values, (uint64_t){11}, &found_ptr);
    if (!found_ptr || eleven == nullptr || eleven->u8_bytes != 7) {
        return 82;
    }
    found_ptr = false;
    twelve = n00b_dict_get(&pointer_values, (uint64_t){12}, &found_ptr);
    if (!found_ptr || twelve == nullptr || twelve->u8_bytes != 6) {
        return 83;
    }
    if (!expect_pointer_value(13, 8)) {
        return 84;
    }
    if (!expect_buffer_key("one", 501)
        || !expect_buffer_key("two", 502)) {
        return 85;
    }
    if (!expect_buffer_value(31, "thirty-one")
        || !expect_buffer_value(32, "thirty-two")) {
        return 86;
    }
    style_alpha_result = expect_style_value(r"alpha", &style_alpha);
    style_beta_result  = expect_style_value(r"beta", &style_beta);
    if (style_alpha_result != 0) {
        return 180 + style_alpha_result;
    }
    if (style_beta_result != 0) {
        return 190 + style_beta_result;
    }

    return 0;
}
