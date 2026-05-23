#include "lib/array.h"

typedef struct opaque_point point_t;

ncc_array_decl(point_t);

const ncc_array_t(point_t) points = [{}];
