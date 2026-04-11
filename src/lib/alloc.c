// alloc.c — Memory debug counter storage.

#include "lib/alloc.h"

#ifdef NCC_MEM_DEBUG
ncc_mem_counters_t ncc_mem_counters = {0};
#endif
