#pragma once

// Canonical definition of ncc_xform_data_t and ncc_meta_table_t.
//
// All transform files include this header instead of re-declaring
// layout-compatible struct copies.  The struct is allocated in ncc.c
// and passed to transforms via ctx->user_data.

#include "xform/xform_template.h"
#include "lib/dict.h"  // ncc_dict_t used in ncc_xform_data_t

#define NCC_META_TABLE_SIZE 256

typedef struct {
    char *key;
    void *value;
} ncc_meta_entry_t;

typedef struct {
    ncc_meta_entry_t entries[NCC_META_TABLE_SIZE];
} ncc_meta_table_t;

typedef struct {
    const char                *compiler;
    const char                *constexpr_headers;
    ncc_meta_table_t           func_meta;
    ncc_dict_t                 option_meta;
    ncc_dict_t                 option_decls;
    ncc_dict_t                 generic_struct_decls;
    ncc_dict_t                 array_types;
    ncc_template_registry_t   *template_reg;
    const char                *vargs_type;
    const char                *once_prefix;
    const char                *rstr_string_type;
    const char                *rstr_text_style_type;
    const char                *rstr_style_record_type;
    const char                *rstr_static_ref_template_styled;
    const char                *rstr_static_ref_template_plain;
    const char                *rstr_static_ref_expr_styled;
    const char                *rstr_static_ref_expr_plain;
    const char                *array_literal_data_template;
    const char                *array_literal_data_expr;
    bool                       gc_stack_maps;
} ncc_xform_data_t;

static inline ncc_xform_data_t *ncc_xform_get_data(ncc_xform_ctx_t *ctx) {
    return (ncc_xform_data_t *)ctx->user_data;
}
