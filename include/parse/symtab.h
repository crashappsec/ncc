#pragma once

/**
 * @file symtab.h
 * @brief Symbol table with named parallel namespaces and push/pop scoping.
 *
 * Each namespace (e.g., "" for variables, "tag" for struct/union/enum,
 * "label" for goto labels) is an independent scope stack. Push/pop
 * operates on one namespace at a time. Designed for post-parse
 * annotation walks on ambiguity-aware parse forests.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"

// ============================================================================
// Symbol kinds
// ============================================================================

typedef enum {
    NCC_SYM_VARIABLE,
    NCC_SYM_FUNCTION,
    NCC_SYM_TYPEDEF,
    NCC_SYM_TAG,
    NCC_SYM_ENUM_CONST,
    NCC_SYM_LABEL,
    NCC_SYM_PARAM,
} ncc_sym_kind_t;

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct ncc_sym_entry_t  ncc_sym_entry_t;
typedef struct ncc_scope_t      ncc_scope_t;
typedef struct ncc_namespace_t  ncc_namespace_t;
typedef struct ncc_symtab_t     ncc_symtab_t;

// ============================================================================
// Symbol entry
// ============================================================================

struct ncc_sym_entry_t {
    ncc_string_t        name;
    ncc_sym_kind_t      kind;
    int32_t              scope_depth;
    ncc_parse_tree_t   *decl_node;
    ncc_sym_entry_t    *shadowed;
    ncc_sym_entry_t    *next_in_scope;
    ncc_parse_tree_t   *type_node;
    ncc_string_t        adt_kind;
    bool                 is_field;
    bool                 is_method;
};

// ============================================================================
// Scope
// ============================================================================

struct ncc_scope_t {
    ncc_scope_t     *parent;
    ncc_string_t     name;
    int32_t           depth;
    ncc_sym_entry_t *first_in_scope;
    ncc_string_t     adt_kind;
};

// ============================================================================
// Namespace
// ============================================================================

struct ncc_namespace_t {
    ncc_string_t     ns_name;
    ncc_scope_t     *current;
    int32_t           depth;
    void             *symbols;  // ncc_dict_t*
};

// ============================================================================
// Symbol table
// ============================================================================

struct ncc_symtab_t {
    ncc_namespace_t *namespaces;
    int32_t           ns_count;
    int32_t           ns_cap;
};

// ============================================================================
// Lifecycle
// ============================================================================

ncc_symtab_t *ncc_symtab_new(void);
void ncc_symtab_free(ncc_symtab_t *st);

// ============================================================================
// Namespace management
// ============================================================================

ncc_namespace_t *ncc_symtab_ns(ncc_symtab_t *st, ncc_string_t ns_name);

// ============================================================================
// Scope management
// ============================================================================

void ncc_symtab_push_scope(ncc_symtab_t *st, ncc_string_t ns_name,
                             ncc_string_t scope_name);
void ncc_symtab_pop_scope(ncc_symtab_t *st, ncc_string_t ns_name);

// ============================================================================
// Symbol operations
// ============================================================================

ncc_sym_entry_t *ncc_symtab_add(ncc_symtab_t *st, ncc_string_t ns_name,
                                    ncc_string_t name, ncc_sym_kind_t kind,
                                    ncc_parse_tree_t *decl_node);

ncc_sym_entry_t *ncc_symtab_lookup(ncc_symtab_t *st, ncc_string_t ns_name,
                                       ncc_string_t name);

bool ncc_symtab_is_typedef(ncc_symtab_t *st, ncc_string_t name);

int32_t ncc_symtab_depth(ncc_symtab_t *st, ncc_string_t ns_name);

ncc_scope_t *ncc_symtab_current_scope(ncc_symtab_t *st,
                                          ncc_string_t ns_name);
