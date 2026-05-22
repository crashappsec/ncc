#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ncc_runtime.h"
#include "lib/array.h"

typedef void (*n00b_static_scan_cb_t)(void *, void *);

typedef struct {
    uint64_t stride;
    uint64_t offset;
    uint64_t count;
} n00b_gc_struct_array_t;

typedef struct {
    const void            *start;
    uint64_t               len;
    uint64_t               tinfo;
    uint8_t                scan_kind;
    n00b_static_scan_cb_t  scan_cb;
    void                  *scan_user;
    uint64_t               object_id;
    const char            *file;
    uint32_t               flags;
} n00b_static_object_desc_t;

static void
n00b_gc_scan_cb_struct_field(void *map, void *range)
{
    (void)map;
    (void)range;
}

ncc_array_decl(int);

typedef ncc_array_t(int) int_array_t;
ncc_array_decl(int_array_t);

typedef ncc_string_t *ncc_string_ptr_t;
ncc_array_decl(ncc_string_ptr_t);

ncc_array_t(int) descriptor_values = [1, 2, 3];

static void
test_scalar_descriptor_literal(void)
{
    assert(descriptor_values.len == 3);
    assert(descriptor_values.data[0] == 1);
    descriptor_values.data[1] = 42;
    assert(descriptor_values.data[1] == 42);
}

static void
test_nested_descriptor_literal(void)
{
    const ncc_array_t(int_array_t) rows = [[1, 2], [3]];

    assert(rows.len == 2);
    assert(rows.data[0].len == 2);
    assert(rows.data[0].data[1] == 2);
    assert(rows.data[1].len == 1);
    assert(rows.data[1].data[0] == 3);
}

static void
test_rstr_descriptor_literals(void)
{
    ncc_string_t *direct = r"direct";
    const ncc_array_t(ncc_string_ptr_t) words =
        [r"alpha", r"«b»hi«/b»"];

    assert(direct->u8_bytes == 6);
    assert(memcmp(direct->data, "direct", 6) == 0);
    assert(words.len == 2);
    assert(words.data[0]->u8_bytes == 5);
    assert(words.data[1]->u8_bytes == 2);
    assert(words.data[1]->styling != nullptr);
}

int
main(void)
{
    test_scalar_descriptor_literal();
    test_nested_descriptor_literal();
    test_rstr_descriptor_literals();
    return 0;
}
