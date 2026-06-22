#include "n00b.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/array.h"
#include "adt/list.h"

typedef n00b_array_t(int) int_array_t;
typedef n00b_list_t(int) int_list_t;

typedef struct {
    n00b_string_t *name;
    n00b_string_t *alias;
} entry_t;

n00b_list_t(int) numbers = l{1, 2, 3};
int_list_t alias_numbers = l{7, 8, 9};
n00b_list_t(n00b_string_t *) words = l{r"red", r"blue"};
n00b_list_t(int_array_t) array_rows = l{[1, 2], [], [3]};
n00b_list_t(int_list_t) rows = l{l{1, 2}, l{}, l{3, 4, 5}};
n00b_list_t(entry_t) entries = l{
    {.name = r"one", .alias = r"uno"},
    {.name = r"two", .alias = r"dos"},
};

int
main(void)
{
    if (numbers.len != 3 || numbers.cap != 16 || numbers.data == nullptr) {
        return 10;
    }
    if (numbers.lock == nullptr || numbers.allocator != nullptr) {
        return 11;
    }
    if (numbers.scan_kind != N00B_GC_SCAN_KIND_NONE
        || numbers.scan_cb != nullptr || numbers.scan_user != nullptr) {
        return 12;
    }
    if (n00b_list_len(numbers) != 3 || n00b_list_get(numbers, 0) != 1
        || n00b_list_get(numbers, 1) != 2 || n00b_list_get(numbers, 2) != 3) {
        return 13;
    }

    n00b_list_set(numbers, 1, 20);
    n00b_list_push(numbers, 4);
    if (numbers.len != 4 || numbers.cap != 16) {
        return 14;
    }
    if (n00b_list_get(numbers, 1) != 20 || n00b_list_get(numbers, 3) != 4) {
        return 15;
    }

    if (alias_numbers.len != 3 || alias_numbers.cap != 16
        || alias_numbers.data == nullptr) {
        return 20;
    }
    if (alias_numbers.lock == nullptr || alias_numbers.allocator != nullptr) {
        return 21;
    }
    if (alias_numbers.scan_kind != N00B_GC_SCAN_KIND_NONE
        || alias_numbers.scan_cb != nullptr
        || alias_numbers.scan_user != nullptr) {
        return 22;
    }
    if (n00b_list_len(alias_numbers) != 3
        || n00b_list_get(alias_numbers, 0) != 7
        || n00b_list_get(alias_numbers, 1) != 8
        || n00b_list_get(alias_numbers, 2) != 9) {
        return 23;
    }
    n00b_list_set(alias_numbers, 0, 70);
    n00b_list_push(alias_numbers, 10);
    if (alias_numbers.len != 4 || alias_numbers.cap != 16) {
        return 24;
    }
    if (n00b_list_get(alias_numbers, 0) != 70
        || n00b_list_get(alias_numbers, 3) != 10) {
        return 25;
    }

    if (words.len != 2 || words.cap != 16 || words.data == nullptr) {
        return 30;
    }
    if (words.lock == nullptr || words.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 31;
    }
    if (words.data[0] == nullptr || words.data[1] == nullptr) {
        return 32;
    }
    if (words.data[0]->u8_bytes != 3 || words.data[1]->u8_bytes != 4) {
        return 33;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (words.data[0] == nullptr || words.data[1] == nullptr) {
        return 34;
    }
    if (words.data[0]->u8_bytes != 3 || words.data[1]->u8_bytes != 4) {
        return 35;
    }

    if (array_rows.len != 3 || array_rows.cap != 16
        || array_rows.data == nullptr) {
        return 36;
    }
    if (array_rows.lock == nullptr
        || array_rows.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 37;
    }
    if (array_rows.data[0].len != 2 || array_rows.data[0].data == nullptr
        || array_rows.data[1].len != 0 || array_rows.data[2].len != 1
        || array_rows.data[2].data == nullptr) {
        return 38;
    }
    if (array_rows.data[0].data[0] != 1 || array_rows.data[0].data[1] != 2
        || array_rows.data[2].data[0] != 3) {
        return 39;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (array_rows.data[0].data == nullptr
        || array_rows.data[2].data == nullptr
        || array_rows.data[0].data[1] != 2
        || array_rows.data[2].data[0] != 3) {
        return 40;
    }

    if (rows.len != 3 || rows.cap != 16 || rows.data == nullptr) {
        return 41;
    }
    if (rows.lock == nullptr || rows.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 42;
    }
    if (rows.data[0].len != 2 || rows.data[0].cap != 16
        || rows.data[0].lock == nullptr || rows.data[0].data == nullptr) {
        return 43;
    }
    if (rows.data[1].len != 0 || rows.data[1].cap != 16
        || rows.data[1].lock == nullptr || rows.data[1].data == nullptr) {
        return 44;
    }
    if (rows.data[2].len != 3 || rows.data[2].cap != 16
        || rows.data[2].lock == nullptr || rows.data[2].data == nullptr) {
        return 45;
    }
    if (n00b_list_get(rows.data[0], 0) != 1
        || n00b_list_get(rows.data[0], 1) != 2
        || n00b_list_get(rows.data[2], 0) != 3
        || n00b_list_get(rows.data[2], 1) != 4
        || n00b_list_get(rows.data[2], 2) != 5) {
        return 46;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (rows.data[0].data == nullptr || rows.data[2].data == nullptr
        || rows.data[0].lock == nullptr || rows.data[2].lock == nullptr) {
        return 47;
    }
    if (n00b_list_get(rows.data[0], 0) != 1
        || n00b_list_get(rows.data[2], 2) != 5) {
        return 48;
    }

    if (entries.len != 2 || entries.cap != 16 || entries.data == nullptr) {
        return 50;
    }
    if (entries.lock == nullptr || entries.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 51;
    }
    if (entries.data[0].name == nullptr || entries.data[1].name == nullptr
        || entries.data[0].alias == nullptr || entries.data[1].alias == nullptr) {
        return 52;
    }
    if (entries.data[0].name->u8_bytes != 3
        || entries.data[1].name->u8_bytes != 3
        || entries.data[0].alias->u8_bytes != 3
        || entries.data[1].alias->u8_bytes != 3) {
        return 53;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (entries.data[0].name == nullptr || entries.data[1].name == nullptr
        || entries.data[0].alias == nullptr || entries.data[1].alias == nullptr) {
        return 54;
    }
    if (entries.data[0].name->u8_bytes != 3
        || entries.data[1].name->u8_bytes != 3
        || entries.data[0].alias->u8_bytes != 3
        || entries.data[1].alias->u8_bytes != 3) {
        return 55;
    }

    return 0;
}
