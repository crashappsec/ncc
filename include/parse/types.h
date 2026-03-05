#pragma once

/**
 * @file types.h
 * @brief Core type definitions for the ncc parser system.
 *
 * Match item types, character classes, parse node structure,
 * forward declarations, callback signatures, and constants.
 */

#include "parse/token.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef int64_t                    ncc_nt_id_t;
typedef struct ncc_terminal_t     ncc_terminal_t;
typedef struct ncc_nonterm_t      ncc_nonterm_t;
typedef struct ncc_parse_rule_t   ncc_parse_rule_t;
typedef struct ncc_rule_group_t   ncc_rule_group_t;
typedef struct ncc_grammar_t      ncc_grammar_t;
typedef struct ncc_earley_item_t  ncc_earley_item_t;
typedef struct ncc_earley_state_t ncc_earley_state_t;

// ============================================================================
// Constants
// ============================================================================

#define NCC_EMPTY_STRING         (-126)
#define NCC_GROUP_ID             (1 << 28)
#define NCC_DEFAULT_MAX_PENALTY  128
#define NCC_ERROR_NODE_ID        (-254)
#define NCC_GID_SHOW_GROUP_LHS  (-255)
#define NCC_IX_START_OF_PROGRAM (-1)

// ============================================================================
// Enums
// ============================================================================

/** @brief Discriminator for grammar match items. */
typedef enum {
    NCC_MATCH_EMPTY,    /**< Epsilon (empty) match. */
    NCC_MATCH_NT,       /**< Non-terminal reference. */
    NCC_MATCH_TERMINAL, /**< Terminal (token or codepoint) match. */
    NCC_MATCH_ANY,      /**< Wildcard: matches any single token. */
    NCC_MATCH_CLASS,    /**< Unicode character class. */
    NCC_MATCH_SET,      /**< Set of alternative match items. */
    NCC_MATCH_GROUP,    /**< Repetition group (?, *, +). */
} ncc_match_kind_t;

/** @brief Unicode character class identifiers. */
typedef enum {
    NCC_CC_ID_START,
    NCC_CC_ID_CONTINUE,
    NCC_CC_ASCII_DIGIT,
    NCC_CC_UNICODE_DIGIT,
    NCC_CC_ASCII_UPPER,
    NCC_CC_ASCII_LOWER,
    NCC_CC_ASCII_ALPHA,
    NCC_CC_WHITESPACE,
    NCC_CC_HEX_DIGIT,
    NCC_CC_NONZERO_DIGIT,
    NCC_CC_PRINTABLE,
    NCC_CC_NON_WS_PRINTABLE,
    NCC_CC_NON_NL_WS,
    NCC_CC_NON_NL_PRINTABLE,
    NCC_CC_JSON_STRING_CHAR,
    NCC_CC_REGEX_BODY_CHAR,
} ncc_char_class_t;

/** @brief Earley algorithm operation codes. */
typedef enum {
    NCC_EO_PREDICT_NT,
    NCC_EO_PREDICT_G,
    NCC_EO_FIRST_GROUP_ITEM,
    NCC_EO_SCAN_TOKEN,
    NCC_EO_SCAN_ANY,
    NCC_EO_SCAN_NULL,
    NCC_EO_SCAN_CLASS,
    NCC_EO_SCAN_SET,
    NCC_EO_COMPLETE_N,
    NCC_EO_ITEM_END,
} ncc_earley_op_t;

/** @brief Subtree boundary tags for parse forest extraction. */
typedef enum {
    NCC_SI_NONE             = 0,
    NCC_SI_NT_RULE_START    = 1,
    NCC_SI_NT_RULE_END      = 2,
    NCC_SI_GROUP_START      = 3,
    NCC_SI_GROUP_END        = 4,
    NCC_SI_GROUP_ITEM_START = 5,
    NCC_SI_GROUP_ITEM_END   = 6,
} ncc_subtree_info_t;

/** @brief Three-way comparison for Earley item cost. */
typedef enum {
    NCC_CMP_EQ = 0,
    NCC_CMP_LT = -1,
    NCC_CMP_GT = 1,
} ncc_earley_cmp_t;

// ============================================================================
// Match item
// ============================================================================

/** @brief A grammar match item (tagged union for rule RHS elements). */
typedef struct ncc_match_t {
    ncc_match_kind_t kind;
    union {
        int64_t           nt_id;
        int64_t           terminal_id;
        ncc_char_class_t char_class;
        void             *set_items;
        void             *group;
    };
} ncc_match_t;

// ============================================================================
// Non-terminal node (interior parse tree node)
// ============================================================================

/** @brief Contents of an interior (non-terminal) parse tree node. */
typedef struct ncc_nt_node_t {
    ncc_string_t name;
    int64_t       noscan;
    int64_t       id;
    int64_t       hv;
    int32_t       start;
    int32_t       end;
    int32_t       rule_index;
    uint32_t      penalty;
    uint32_t      cost;
    uint16_t      penalty_location;
    bool          group_item;
    bool          group_top;
    bool          missing;
    bool          bad_prefix;
    void         *parent;
} ncc_nt_node_t;

// ============================================================================
// Callbacks
// ============================================================================

/** @brief Walk action callback invoked for each NT during tree walk. */
typedef void *(*ncc_walk_action_t)(ncc_nt_node_t *node,
                                     void *children,
                                     void *thunk);

/** @brief Tree walker callback. */
typedef bool (*ncc_tree_walker_fn)(void *node, int32_t depth, void *thunk);
