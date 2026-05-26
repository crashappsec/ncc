#include "lib/array.h"

typedef union {
    void *ptr;
    int   tag;
} union_item_t;

ncc_array_decl(union_item_t);

const ncc_array_t(union_item_t) items = [{.ptr = nullptr}];
