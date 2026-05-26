#include "lib/array.h"

enum {
    SLOT_COUNT = 2,
};

typedef struct {
    void *slots[SLOT_COUNT];
    int   tag;
} slot_item_t;

ncc_array_decl(slot_item_t);

const ncc_array_t(slot_item_t) items = [{
    .slots = {nullptr, nullptr},
    .tag   = 1,
}];
