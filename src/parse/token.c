// token.c - Token list operations.
#include "parse/token.h"
#include <assert.h>

int32_t
ncc_token_list_build_ptrs(ncc_list_t(ncc_token_info_t) *tl, ncc_token_info_ptr_t **out)
{
    assert(tl);
    assert(out);

    int32_t n = (int32_t)tl->len;

    ncc_token_info_ptr_t *ptrs = ncc_alloc_array(ncc_token_info_ptr_t, n);

    for (int32_t i = 0; i < n; i++) {
        ptrs[i] = &tl->data[i];
    }

    *out = ptrs;
    return n;
}
