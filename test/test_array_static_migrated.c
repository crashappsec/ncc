#include "n00b.h"
#include "core/alloc.h"
#include "core/gc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/array.h"

typedef n00b_array_t(int) int_array_t;

typedef struct {
    n00b_string_t *name;
    n00b_string_t *alias;
} entry_t;

n00b_array_t(int) state = [2, 4, 6, 8];
int_array_t alias_state = [9, 10, 11];
n00b_array_t(n00b_string_t *) words = [r"alpha", r"beta"];
n00b_array_t(int_array_t) rows = [[1, 2], [], [3, 4, 5]];
n00b_array_t(entry_t) entries = [
    {.name = r"one", .alias = r"uno"},
    {.name = r"two", .alias = r"dos"},
];

int
main(void)
{
    if (state.len != 4 || state.cap != 4 || state.data == nullptr) {
        return 10;
    }
    if (state.data[0] != 2 || state.data[1] != 4
        || state.data[2] != 6 || state.data[3] != 8) {
        return 11;
    }
    if (state.lock != nullptr || state.allocator != nullptr) {
        return 12;
    }
    if (state.scan_kind != N00B_GC_SCAN_KIND_NONE
        || state.scan_cb != nullptr || state.scan_user != nullptr) {
        return 13;
    }

    state.data[1] = 40;
    if (state.data[1] != 40) {
        return 14;
    }

    if (alias_state.len != 3 || alias_state.cap != 3
        || alias_state.data == nullptr) {
        return 20;
    }
    if (alias_state.data[0] != 9 || alias_state.data[1] != 10
        || alias_state.data[2] != 11) {
        return 21;
    }
    if (alias_state.lock != nullptr || alias_state.allocator != nullptr) {
        return 22;
    }
    if (alias_state.scan_kind != N00B_GC_SCAN_KIND_NONE
        || alias_state.scan_cb != nullptr || alias_state.scan_user != nullptr) {
        return 23;
    }
    alias_state.data[2] = 110;
    if (alias_state.data[2] != 110) {
        return 24;
    }

    if (words.len != 2 || words.cap != 2 || words.data == nullptr) {
        return 30;
    }
    if (words.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 31;
    }
    if (words.data[0] == nullptr || words.data[1] == nullptr) {
        return 32;
    }
    if (words.data[0]->u8_bytes != 5 || words.data[1]->u8_bytes != 4) {
        return 33;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (words.data[0] == nullptr || words.data[1] == nullptr) {
        return 34;
    }
    if (words.data[0]->u8_bytes != 5 || words.data[1]->u8_bytes != 4) {
        return 35;
    }

    if (rows.len != 3 || rows.cap != 3 || rows.data == nullptr) {
        return 40;
    }
    if (rows.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 41;
    }
    if (rows.data[0].len != 2 || rows.data[0].cap != 2
        || rows.data[0].data == nullptr) {
        return 42;
    }
    if (rows.data[1].len != 0 || rows.data[1].cap != 0) {
        return 43;
    }
    if (rows.data[2].len != 3 || rows.data[2].cap != 3
        || rows.data[2].data == nullptr) {
        return 44;
    }
    if (rows.data[0].data[0] != 1 || rows.data[0].data[1] != 2
        || rows.data[2].data[0] != 3 || rows.data[2].data[1] != 4
        || rows.data[2].data[2] != 5) {
        return 45;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (rows.data[0].data == nullptr || rows.data[2].data == nullptr) {
        return 46;
    }
    if (rows.data[0].data[0] != 1 || rows.data[0].data[1] != 2
        || rows.data[2].data[0] != 3 || rows.data[2].data[1] != 4
        || rows.data[2].data[2] != 5) {
        return 47;
    }

    if (entries.len != 2 || entries.cap != 2 || entries.data == nullptr) {
        return 50;
    }
    if (entries.scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 51;
    }
    if (entries.data[0].alias == nullptr || entries.data[1].alias == nullptr) {
        return 52;
    }
    if (entries.data[0].name == nullptr || entries.data[1].name == nullptr) {
        return 53;
    }
    if (entries.data[0].name->u8_bytes != 3
        || entries.data[1].name->u8_bytes != 3) {
        return 54;
    }
    if (entries.data[0].alias->u8_bytes != 3
        || entries.data[1].alias->u8_bytes != 3) {
        return 57;
    }

    n00b_collect(n00b_get_runtime()->default_arena);

    if (entries.data[0].name == nullptr || entries.data[1].name == nullptr) {
        return 55;
    }
    if (entries.data[0].alias == nullptr || entries.data[1].alias == nullptr) {
        return 58;
    }
    if (entries.data[0].name->u8_bytes != 3
        || entries.data[1].name->u8_bytes != 3) {
        return 56;
    }
    if (entries.data[0].alias->u8_bytes != 3
        || entries.data[1].alias->u8_bytes != 3) {
        return 59;
    }

    return 0;
}
