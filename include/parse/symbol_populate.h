#pragma once

/**
 * @file symbol_populate.h
 * @brief Build a scoped symbol table from a parsed translation unit.
 *
 * A post-parse, scope-aware DFS that records every named entity — typedefs,
 * variables, functions, parameters, struct/union/enum tags, and enum
 * constants — into an `ncc_symtab_t`, along with the parse subtree describing
 * its type. This is the foundation the type model queries: expression typing
 * (`typeof`, `typehash`/`typeid` of expressions) and the transforms that today
 * pattern-match parse-tree shapes resolve names through this table instead.
 *
 * Each symbol carries:
 *   - `decl_node`: the declarator (or specifier) parse node it was declared by.
 *   - `type_node`: the `declaration_specifiers` / `specifier_qualifier_list`
 *     subtree giving the base type; combined with the declarator's pointer/
 *     array suffix to derive the full type.
 *
 * Ordinary names live in the "" namespace; struct/union/enum tags live in the
 * "tag" namespace. Scopes are pushed at `function_definition` and
 * `compound_statement` boundaries.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

/**
 * @brief Walk @p tree and return a populated symbol table (caller frees with
 *        `ncc_symtab_free`). @p g supplies the IDENTIFIER terminal id used to
 *        locate declared names.
 */
ncc_symtab_t *ncc_populate_symbols(ncc_grammar_t *g, ncc_parse_tree_t *tree);
