#include "lib/array.h"

ncc_array_decl(int);

typedef ncc_array_t(int) int_array_t;
ncc_array_decl(int_array_t);

const ncc_array_t(int_array_t) rows = [1, 2, 3];
