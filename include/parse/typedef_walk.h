#pragma once

/**
 * @file typedef_walk.h
 * @brief Post-parse typedef reclassification walk.
 *
 * DFS post-order walk that finds `typedef` declarations and
 * reclassifies subsequent IDENTIFIER tokens to TYPEDEF_NAME.
 * Scope-aware: compound statements and function definitions
 * limit reclassification range.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"

/**
 * @brief Walk a parse tree, reclassifying identifiers declared via
 *        `typedef` to TYPEDEF_NAME.
 *
 * Hardcoded for the C grammar: looks for `<declaration>` nodes whose
 * `<declaration_specifiers>` subtree contains a "typedef" token,
 * then extracts IDENTIFIER tokens from `<init_declarator_list>` and
 * mutates all matching tokens after the declaration.
 *
 * @param g       Grammar (used for NT name lookup).
 * @param tree    Root of the parse tree to walk.
 * @param tokens  Array of token pointers (from token stream).
 * @param ntokens Number of tokens in the array.
 * @return Number of tokens reclassified.
 */
int32_t ncc_typedef_walk(ncc_grammar_t    *g,
                         ncc_parse_tree_t *tree,
                         ncc_token_info_t **tokens,
                         int32_t            ntokens);
