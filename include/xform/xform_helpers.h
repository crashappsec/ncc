#pragma once

// Shared transform helper utilities.

#include "xform/transform.h"
#include "util/type_normalize.h"

// Check if a non-terminal node has the given name.
bool ncc_xform_nt_name_is(ncc_parse_tree_t *node, const char *name);

// Find a direct child with the given NT name, transparently unwrapping
// group wrapper nodes ($$group_*). Returns nullptr if not found.
ncc_parse_tree_t *ncc_xform_find_child_nt(ncc_parse_tree_t *node,
                                              const char        *name);

// Get token text from a leaf node. Returns nullptr if not a leaf or no value.
const char *ncc_xform_leaf_text(ncc_parse_tree_t *node);

// Check if a leaf token's text matches a C string.
bool ncc_xform_leaf_text_eq(ncc_parse_tree_t *node, const char *text);

// Collect type atoms from typeid/typestr/typehash argument structure.
// Walks <typeid_atom> and <typeid_continuation> children.
// Returns an ncc_string_t with the combined canonical type string.
ncc_string_t ncc_xform_extract_type_string(ncc_xform_ctx_t  *ctx,
                                            ncc_parse_tree_t *atom_node,
                                            ncc_parse_tree_t *cont_node);

// Get line/column from first leaf token in a subtree.
void ncc_xform_first_leaf_pos(ncc_parse_tree_t *node,
                                uint32_t *line, uint32_t *col);

// Walk all leaves of a subtree and concatenate their text with spaces.
// Returns an ncc_string_t.
ncc_string_t ncc_xform_node_to_text(ncc_parse_tree_t *node);

// Parse a source string into a subtree (wraps ncc_xform_parse_template
// with error reporting).  Returns nullptr on failure.
ncc_parse_tree_t *ncc_xform_parse_source(ncc_grammar_t *g, const char *nt_name,
                                          const char *src, const char *xform_name);

// Find the rightmost leaf token in a subtree.
ncc_token_info_t *ncc_xform_find_last_leaf_token(ncc_parse_tree_t *node);

// Walk to the leftmost leaf and return its text.
const char *ncc_xform_get_first_leaf_text(ncc_parse_tree_t *node);

// Check if declaration_specifiers contain a void type
// (skipping storage_class_specifier and function_specifier).
bool ncc_xform_has_void_type(ncc_parse_tree_t *decl_specs);

// Collect base type text from declaration_specifiers (type specifiers only,
// skip storage-class and function specifiers).
// Returns heap-allocated string. Caller frees.
char *ncc_xform_collect_base_type(ncc_parse_tree_t *decl_specs);

// Collect qualifiers from declaration_specifiers (storage-class +
// function specifiers, optionally skipping a named specifier like "_Once").
// Pass skip=nullptr to include all. Returns heap-allocated string.
char *ncc_xform_collect_qualifiers(ncc_parse_tree_t *decl_specs,
                                    const char *skip);

// Shared helpers from xform_constexpr.c (non-static, used by constexpr_paste
// and option transforms).
char *compile_and_run(const char *compiler, const char *source,
                      char **err_out);
ncc_string_t pprint_subtree(ncc_grammar_t *g, ncc_parse_tree_t *node);
ncc_parse_tree_t **collect_arguments(ncc_parse_tree_t *arglist, int *nargs);
char *collect_file_scope_declarations(ncc_xform_ctx_t *ctx,
                                      ncc_parse_tree_t *call_node);
