#pragma once

// Shared C type-layout helpers for transforms that need to reason about
// pointer-bearing aggregate fields.

#include "xform/transform.h"

#include <stdint.h>

typedef struct {
    ncc_parse_tree_t **data;
    size_t             len;
    size_t             cap;
} ncc_layout_parse_tree_list_t;

typedef struct {
    uint64_t *data;
    size_t    len;
    size_t    cap;
} ncc_layout_uint64_list_t;

typedef struct {
    char             *key;
    ncc_parse_tree_t *specifier;
    char             *offset_type;
    bool              is_atomic;
    bool              is_union;
} ncc_layout_aggregate_type_info_t;

char *ncc_layout_copy_cstr(const char *s);
char *ncc_layout_trim_copy(const char *s);
char *ncc_layout_format_cstr(const char *fmt, ...);
char *ncc_layout_node_text(ncc_parse_tree_t *node);

void ncc_layout_parse_tree_list_push(ncc_layout_parse_tree_list_t *list,
                                     ncc_parse_tree_t *node);
void ncc_layout_uint64_list_push(ncc_layout_uint64_list_t *list,
                                 uint64_t value);
void ncc_layout_collect_nt_children(ncc_parse_tree_t *node, const char *name,
                                    ncc_layout_parse_tree_list_t *out);

ncc_parse_tree_t *ncc_layout_first_descendant_nt(ncc_parse_tree_t *node,
                                                 const char *name);
ncc_token_info_t *ncc_layout_first_leaf_token(ncc_parse_tree_t *node);
bool ncc_layout_node_starts_in_system_header(ncc_parse_tree_t *node);

bool ncc_layout_parse_positive_integer_literal(const char *text,
                                               uint64_t *out);
bool ncc_layout_has_unbounded_array_declarator(ncc_parse_tree_t *node);
bool ncc_layout_array_declarator_dimensions(
    ncc_parse_tree_t *declarator, ncc_layout_uint64_list_t *dims,
    ncc_parse_tree_t **bad_array);
bool ncc_layout_array_literal_dimensions(ncc_parse_tree_t *declarator,
                                         ncc_parse_tree_t *decl_node,
                                         ncc_layout_uint64_list_t *dims);
char *ncc_layout_num_words_expr_for_pointer_array(
    ncc_parse_tree_t *declarator, const char *expr,
    ncc_parse_tree_t **bad_array);

char *ncc_layout_declarator_name(ncc_parse_tree_t *node);
char *ncc_layout_first_typedef_name_text(ncc_parse_tree_t *node);
int ncc_layout_pointer_depth_in_declarator(ncc_parse_tree_t *node);
int ncc_layout_pointer_depth_for_specs(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *specs);
int ncc_layout_pointer_depth_for_declarator(ncc_xform_ctx_t *ctx,
                                            ncc_parse_tree_t *specs,
                                            ncc_parse_tree_t *declarator);
bool ncc_layout_declarator_is_function_pointer(ncc_parse_tree_t *declarator);

bool ncc_layout_struct_or_union_is_union(ncc_parse_tree_t *su);
bool ncc_layout_aggregate_specifier_has_members(ncc_parse_tree_t *su);
char *ncc_layout_aggregate_key_from_specifier(ncc_parse_tree_t *su);
ncc_parse_tree_t *ncc_layout_resolve_aggregate_specifier(
    ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su);
ncc_parse_tree_t *ncc_layout_aggregate_spec_from_specs(
    ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs);
ncc_layout_aggregate_type_info_t *ncc_layout_lookup_aggregate_type(
    ncc_xform_ctx_t *ctx, const char *key);
ncc_layout_aggregate_type_info_t *ncc_layout_aggregate_info_from_specs(
    ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs);
ncc_layout_aggregate_type_info_t *ncc_layout_aggregate_info_from_type_name(
    ncc_xform_ctx_t *ctx, const char *type_name);

bool ncc_layout_specs_have_atomic_type_wrapper(ncc_parse_tree_t *node);
char *ncc_layout_aggregate_offset_type_from_specs(ncc_xform_ctx_t *ctx,
                                                  ncc_parse_tree_t *specs);
char *ncc_layout_implicit_member_field_name(ncc_parse_tree_t *member,
                                            ncc_parse_tree_t *member_specs);

bool ncc_layout_typedef_name_is_pointer(ncc_xform_ctx_t *ctx,
                                        const char *name);
// True if `name` is a typedef for a FUNCTION pointer (code pointer). Used to
// exclude such fields from GC pointer maps.
bool ncc_layout_typedef_name_is_function_pointer(ncc_xform_ctx_t *ctx,
                                                 const char *name);
void ncc_layout_collect_type_info(ncc_xform_ctx_t *ctx,
                                  ncc_parse_tree_t *tu);
