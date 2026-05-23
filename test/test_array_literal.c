#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "lib/array.h"
#include "ncc_runtime.h"

ncc_array_decl(int);

typedef ncc_array_t(int) int_array_t;
ncc_array_decl(int_array_t);

typedef ncc_string_t *ncc_string_ptr_t;
ncc_array_decl(ncc_string_ptr_t);

typedef void *generic_ptr_t;
ncc_array_decl(generic_ptr_t);

typedef struct {
    ncc_string_ptr_t label;
    int              code;
} labeled_item_t;
ncc_array_decl(labeled_item_t);

typedef struct {
    ncc_string_ptr_t labels[2];
    int              counts[2];
} label_group_t;
ncc_array_decl(label_group_t);

typedef struct {
    int x;
    int y;
} point_t;
ncc_array_decl(point_t);

typedef struct {
    void     *left;
    uintptr_t scalar;
    void     *right;
} sparse_item_t;
ncc_array_decl(sparse_item_t);

ncc_array_t(int) module_values = [1, 2, 3];

static void
test_module_array(void)
{
    assert(module_values.len == 3);
    assert(module_values.cap == 3);
    assert(module_values.data[0] == 1);
    assert(module_values.data[1] == 2);
    assert(module_values.data[2] == 3);

    module_values.data[1] = 20;
    assert(module_values.data[1] == 20);
}

static void
test_local_const_array(void)
{
    const ncc_array_t(int) local = [4, 5, 6,];

    assert(local.len == 3);
    assert(local.cap == 3);
    assert(local.data[0] == 4);
    assert(local.data[1] == 5);
    assert(local.data[2] == 6);
}

static void
test_empty_array(void)
{
    const ncc_array_t(int) empty = [];

    assert(empty.data == nullptr);
    assert(empty.len == 0);
    assert(empty.cap == 0);
}

static void
test_nested_arrays(void)
{
    const ncc_array_t(int_array_t) rows = [[1, 2], [], [3, 4, 5]];

    assert(rows.len == 3);
    assert(rows.cap == 3);

    assert(rows.data[0].len == 2);
    assert(rows.data[0].data[0] == 1);
    assert(rows.data[0].data[1] == 2);

    assert(rows.data[1].data == nullptr);
    assert(rows.data[1].len == 0);
    assert(rows.data[1].cap == 0);

    assert(rows.data[2].len == 3);
    assert(rows.data[2].data[0] == 3);
    assert(rows.data[2].data[1] == 4);
    assert(rows.data[2].data[2] == 5);
}

static void
test_rstring_array(void)
{
    const ncc_array_t(ncc_string_ptr_t) words =
        [r"alpha", r"beta", r"«b»hi«/b»"];

    assert(words.len == 3);
    assert(words.cap == 3);
    assert(words.data[0] != nullptr);
    assert(words.data[1] != nullptr);
    assert(words.data[2] != nullptr);
    assert(words.data[0]->u8_bytes == 5);
    assert(words.data[1]->u8_bytes == 4);
    assert(words.data[2]->u8_bytes == 2);
    assert(memcmp(words.data[0]->data, "alpha", 5) == 0);
    assert(memcmp(words.data[1]->data, "beta", 4) == 0);
    assert(memcmp(words.data[2]->data, "hi", 2) == 0);
    assert(words.data[2]->styling != nullptr);
}

static void
test_pointer_typedef_array(void)
{
    static int marker = 42;
    const ncc_array_t(generic_ptr_t) ptrs = [&marker, nullptr];

    assert(ptrs.len == 2);
    assert(ptrs.cap == 2);
    assert(ptrs.data[0] == &marker);
    assert(ptrs.data[1] == nullptr);
}

static void
test_aggregate_arrays(void)
{
    const ncc_array_t(point_t) points =
        [{.x = 1, .y = 2}, {.x = 3, .y = 4}];
    const ncc_array_t(sparse_item_t) items =
        [{.left = (void *)0x10, .scalar = 17, .right = (void *)0x20}];
    const ncc_array_t(labeled_item_t) labels =
        [{.label = r"label", .code = 7},
         {.label = r"«i»ok«/i»", .code = 8}];
    const ncc_array_t(label_group_t) groups =
        [{.labels = {r"left", r"right"}, .counts = {1, 2}}];

    assert(points.len == 2);
    assert(points.data[0].x == 1);
    assert(points.data[1].y == 4);
    assert(items.len == 1);
    assert(items.data[0].left == (void *)0x10);
    assert(items.data[0].scalar == 17);
    assert(items.data[0].right == (void *)0x20);
    assert(labels.len == 2);
    assert(labels.data[0].code == 7);
    assert(labels.data[0].label->u8_bytes == 5);
    assert(memcmp(labels.data[0].label->data, "label", 5) == 0);
    assert(labels.data[1].code == 8);
    assert(labels.data[1].label->u8_bytes == 2);
    assert(labels.data[1].label->styling != nullptr);
    assert(groups.len == 1);
    assert(groups.data[0].labels[0]->u8_bytes == 4);
    assert(memcmp(groups.data[0].labels[0]->data, "left", 4) == 0);
    assert(groups.data[0].labels[1]->u8_bytes == 5);
    assert(groups.data[0].counts[0] == 1);
    assert(groups.data[0].counts[1] == 2);
}

int
main(void)
{
    test_module_array();
    test_local_const_array();
    test_empty_array();
    test_nested_arrays();
    test_rstring_array();
    test_pointer_typedef_array();
    test_aggregate_arrays();

    return 0;
}
