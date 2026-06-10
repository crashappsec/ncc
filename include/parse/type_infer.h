#pragma once

/**
 * @file type_infer.h
 * @brief Expression type inference over the scoped symbol table.
 *
 * Computes the C type of an expression parse subtree, returned as a canonical
 * type spelling (e.g. "int", "n00b_string_t*", "struct foo*"). The spelling is
 * normalized the same way `ncc_normalize_type_tree` normalizes written types,
 * so it is directly usable as a `typeof` replacement AND hashes identically to
 * the same type written explicitly (`typehash`/`typeid`/`typestr`).
 *
 * Coverage is incremental. Supported forms return a heap string (caller frees);
 * anything not yet handled returns nullptr, so callers fall back to existing
 * behavior rather than emitting a wrong type.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

/**
 * @brief Canonical type spelling of expression @p expr under symbol table
 *        @p st, or nullptr if the form is not yet typeable. Caller frees.
 */
char *ncc_type_of_expr(ncc_symtab_t *st, ncc_parse_tree_t *expr);

/**
 * @brief Resolve a type name (e.g. "foo_t", "struct foo") to its aggregate
 *        specifier carrying a member body, via the symbol table (following a
 *        typedef to its underlying tag). Returns the specifier or nullptr.
 *        Resolves types the legacy aggregate table misses, notably
 *        `_generic_struct` typedefs.
 */
ncc_parse_tree_t *ncc_symtab_aggregate_spec(ncc_symtab_t *st,
                                            const char *type_name);
