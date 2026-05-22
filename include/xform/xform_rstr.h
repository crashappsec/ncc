#pragma once

#include "xform/transform.h"

typedef struct {
    char *decl;
    char *expr;
} ncc_rstr_static_ref_t;

ncc_rstr_static_ref_t ncc_rstr_build_static_ref(ncc_xform_ctx_t *ctx,
                                                ncc_parse_tree_t *node);
