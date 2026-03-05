#pragma once

/**
 * @file parse_tree.h
 * @brief Parse tree type and node inspection helpers.
 *
 * Defines the concrete parse tree type shared by the parser engines
 * (PWZ, Earley). Interior nodes carry @c ncc_nt_node_t and
 * leaves carry @c ncc_token_info_t pointers.
 */

#include "parse/types.h"
#include "lib/tree.h"

// ============================================================================
// Parse tree type
// ============================================================================

ncc_tree_decl(ncc_nt_node_t, ncc_token_info_ptr_t);

/**
 * @brief A parse tree: n-ary tree where interior nodes carry
 *        @c ncc_nt_node_t and leaves carry @c ncc_token_info_t pointers.
 */
typedef ncc_tree_t(ncc_nt_node_t, ncc_token_info_ptr_t) ncc_parse_tree_t;

// ============================================================================
// Parse tree pointer type + array type
// ============================================================================

typedef ncc_parse_tree_t *ncc_parse_tree_ptr_t;

ncc_array_decl(ncc_parse_tree_ptr_t);

/** @brief Array of parse tree pointers. */
typedef ncc_array_t(ncc_parse_tree_ptr_t) ncc_parse_tree_array_t;

// ============================================================================
// Quick accessors
// ============================================================================

/** @brief Get the NT id from a parse tree node (-1 if leaf or nullptr). */
static inline int64_t
ncc_pt_nt_id(ncc_parse_tree_t *t)
{
    if (!t || ncc_tree_is_leaf(t)) {
        return -1;
    }
    return ncc_tree_node_value(t).id;
}

/** @brief Get the number of children (0 for leaves or nullptr). */
static inline size_t
ncc_pt_num_children(ncc_parse_tree_t *t)
{
    if (!t) {
        return 0;
    }
    return ncc_tree_num_children(t);
}

// ============================================================================
// Node inspection (engine-agnostic)
// ============================================================================

/**
 * @brief Check whether a parse tree node represents a terminal token.
 * @param t  Parse tree node to inspect.
 * @return True if the node is a terminal (token) leaf.
 */
bool ncc_parse_node_is_token(ncc_parse_tree_t *t);

/**
 * @brief Get the token from a terminal parse tree node.
 * @param t  Parse tree node (must be a terminal leaf).
 * @return Pointer to the token info, or nullptr if not a terminal.
 */
ncc_token_info_t *ncc_parse_node_token(ncc_parse_tree_t *t);

/**
 * @brief Get the non-terminal name from a parse tree node.
 * @param t  Parse tree node (must be a non-terminal interior node).
 * @return The NT name, or none if the node is a terminal.
 */
ncc_option_t(ncc_string_t) ncc_parse_node_name(ncc_parse_tree_t *t);

// ============================================================================
// Tree walking (engine-agnostic)
// ============================================================================

/**
 * @brief Walk a parse tree using grammar-registered actions.
 *
 * Performs a depth-first traversal. For each non-terminal node, calls
 * the walk action registered on the grammar (or the default action).
 * Terminal nodes return their token pointer directly.
 *
 * @param g      Grammar with registered walk actions.
 * @param node   Root of the subtree to walk.
 * @param thunk  User-defined context pointer passed to actions.
 * @return The result value from the root action.
 */
void *ncc_parse_tree_walk(ncc_grammar_t *g, ncc_parse_tree_t *node, void *thunk);

/**
 * @brief Recursively free a parse tree.
 *
 * Frees all tree node structures. Does not free the tokens
 * themselves (those are owned by the token list / parser).
 *
 * @param t  Root of the tree to free.
 */
void ncc_parse_tree_free(ncc_parse_tree_t *t);
