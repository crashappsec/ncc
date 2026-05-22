#include "lib/array.h"

typedef struct {
    int x;
} point_t;

ncc_array_decl(point_t);

const ncc_array_t(point_t) points = [{.x = 1}];
