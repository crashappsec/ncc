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
    bool                 is_comptime;
    // WP-005: file-scope/static-storage initializer that routes through
    // static-init baking metadata. Mutually exclusive with is_comptime.
    bool                 is_static_init;
    // True when baking the initializer requires host execution and can later
    // use the cross-target degrade path instead of a host-produced image.
    bool                 static_init_needs_host_exec;
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
    // The parse node that created this scope (function_definition /
    // compound_statement), or nullptr. Used to map an expression back to its
    // lexical scope for scoped lookup. Set via ncc_symtab_set_scope_node.
    ncc_parse_tree_t *node;
    // Retained-scope chain: every scope ever pushed is linked here so the full
    // scope tree survives until the symtab is freed (scopes are no longer freed
    // on pop), enabling location-based lexical lookup after population.
    ncc_scope_t     *all_next;
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
    // Head of the retained-scope list (every pushed scope, all namespaces).
    ncc_scope_t     *all_scopes;
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

// Tag the current scope of namespace @p ns_name with the parse node that
// created it (function_definition / compound_statement).
void ncc_symtab_set_scope_node(ncc_symtab_t *st, ncc_string_t ns_name,
                               ncc_parse_tree_t *node);

// The retained scope created by parse node @p node (or nullptr). Scopes are
// kept after population, so this works post-walk.
ncc_scope_t *ncc_symtab_scope_for_node(ncc_symtab_t *st,
                                       ncc_parse_tree_t *node);

// Lexical lookup starting from @p scope, walking up the parent-scope chain;
// falls back to a global (file-scope) lookup in namespace @p ns_name. A null
// @p scope is a plain global lookup.
ncc_sym_entry_t *ncc_symtab_lookup_scoped(ncc_symtab_t *st, ncc_scope_t *scope,
                                          ncc_string_t ns_name,
                                          ncc_string_t name);

bool ncc_symtab_is_typedef(ncc_symtab_t *st, ncc_string_t name);

int32_t ncc_symtab_depth(ncc_symtab_t *st, ncc_string_t ns_name);

ncc_scope_t *ncc_symtab_current_scope(ncc_symtab_t *st,
                                          ncc_string_t ns_name);
