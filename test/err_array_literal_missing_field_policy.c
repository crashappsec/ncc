#include "lib/array.h"

typedef _Complex double complex_value_t;

typedef struct {
    complex_value_t value;
    int             tag;
} complex_item_t;

ncc_array_decl(complex_item_t);

const ncc_array_t(complex_item_t) items = [{.value = 1.0, .tag = 1}];
