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
#include "parse/token.h"

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

/**
 * @brief `@disambig` predicate evaluator for type-aware parse disambiguation
 *        (TS-5). Signature matches ncc_pwz_disambig_fn: @p ud is the
 *        `ncc_symtab_t *`. Returns true when @p predicate holds for @p tok's
 *        identifier text:
 *          - "id_is_value":   names a variable, parameter, or function
 *          - "id_is_typedef": names a typedef
 *        so the extractor can prefer the value-expression vs type reading.
 */
bool ncc_disambig_eval(void *ud, const char *predicate, ncc_token_info_t *tok);
