#pragma once

// Canonical definition of ncc_xform_data_t.
//
// All transform files include this header instead of re-declaring
// layout-compatible struct copies.  The struct is allocated in ncc.c
// and passed to transforms via ctx->user_data.

#include "xform/xform_template.h"
#include "lib/dict.h"  // ncc_dict_t used in ncc_xform_data_t

typedef enum {
    NCC_GC_STACK_ROOT_PARAM,
    NCC_GC_STACK_ROOT_LOCAL,
} ncc_gc_stack_root_kind_t;

typedef enum {
    NCC_GC_STACK_ROOT_POINTER,
    NCC_GC_STACK_ROOT_POINTER_ARRAY,
    NCC_GC_STACK_ROOT_AGGREGATE,
} ncc_gc_stack_root_shape_t;

typedef struct ncc_gc_stack_root_t {
    char                       *function_name;
    char                       *name;
    char                       *type_text;
    char                       *address_expr;
    char                       *num_words_expr;
    ncc_gc_stack_root_kind_t    kind;
    ncc_gc_stack_root_shape_t   shape;
    uint64_t                    num_words;
    uint32_t                    line;
    uint32_t                    col;
    // True when num_words_expr is a runtime-evaluated expression (e.g.,
    // sizeof(some_VLA)/sizeof(void *)) rather than a compile-time integer
    // constant.  The slot table / map for any group containing such a root
    // must be emitted as block-scope (not `static const`) so the slot's
    // num_words initializer can reference the VLA's runtime size.
    bool                        num_words_runtime;
    ncc_parse_tree_t           *scope;
    ncc_parse_tree_t           *declaration;
    struct ncc_gc_stack_root_t *next;
} ncc_gc_stack_root_t;

typedef struct {
    const char                *compiler;
    const char                *input_file;
    const char                *constexpr_headers;
    ncc_dict_t                 func_meta;
    ncc_dict_t                 option_meta;
    ncc_dict_t                 option_decls;
    ncc_dict_t                 generic_struct_decls;
    ncc_dict_t                 array_types;
    ncc_dict_t                 list_types;
    ncc_dict_t                 dict_types;
    ncc_dict_t                 gc_aggregate_types;
    ncc_dict_t                 gc_pointer_typedefs;
    // Subset of gc_pointer_typedefs that are FUNCTION pointers (code, not
    // heap). The gc-typemap walker excludes these from GC pointer maps.
    ncc_dict_t                 gc_function_pointer_typedefs;
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
    const char                *static_object_entry_attr;
    const char                *static_init_helper;
    bool                       static_identity_generate_namespace;
    bool                       gc_stack_maps;
    bool                       gc_stack_maps_relaxed;
    bool                       auto_gc_roots;
    ncc_gc_stack_root_t       *gc_stack_roots;
    size_t                     gc_stack_root_count;
} ncc_xform_data_t;

static inline ncc_xform_data_t *ncc_xform_get_data(ncc_xform_ctx_t *ctx) {
    return (ncc_xform_data_t *)ctx->user_data;
}
