#include "lib/array.h"

typedef int n00b_fd_t;

typedef struct {
    n00b_fd_t fd;
    int       tag;
} fd_item_t;

ncc_array_decl(fd_item_t);

const ncc_array_t(fd_item_t) items = [{.fd = 3, .tag = 1}];
