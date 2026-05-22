#include <assert.h>
#include <string.h>

#include "lib/array.h"
#include "ncc_runtime.h"

ncc_array_decl(int);

typedef ncc_array_t(int) int_array_t;
ncc_array_decl(int_array_t);

typedef ncc_string_t *ncc_string_ptr_t;
ncc_array_decl(ncc_string_ptr_t);

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

int
main(void)
{
    test_module_array();
    test_local_const_array();
    test_empty_array();
    test_nested_arrays();
    test_rstring_array();

    return 0;
}
