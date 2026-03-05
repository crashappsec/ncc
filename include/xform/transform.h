#pragma once

/**
 * @file transform.h
 * @brief Tree transform framework: registry, pre/post-order transforms,
 *        tree mutation, node construction, and pattern search.
 *
 * Provides the infrastructure for semantic tree transforms (e.g., ncc's
 * 12 AST transforms). Transforms are registered by NT name or id, then
 * applied in one or more passes over a parse tree.
 *
 * ### Usage
 *
 * ```c
 * ncc_xform_registry_t reg;
 * ncc_xform_registry_init(&reg, grammar);
 * ncc_xform_register(&reg, "function_definition", my_xform, "my_xform");
 *
 * ncc_xform_ctx_t ctx;
 * ncc_xform_ctx_init(&ctx, grammar, &reg, tree);
 * tree = ncc_xform_apply(&reg, &ctx);
 *
 * ncc_xform_registry_free(&reg);
 * ```
 */

#include "parse/parse_tree.h"
#include "scanner/scanner.h"
#include "lib/result.h"

ncc_result_decl(ncc_parse_tree_ptr_t);

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct ncc_xform_registry_t ncc_xform_registry_t;
typedef struct ncc_xform_ctx_t      ncc_xform_ctx_t;

// ============================================================================
// Control types
// ============================================================================

/** @brief Control value returned by pre-order transforms. */
typedef enum {
    NCC_XFORM_CONTINUE,      /**< Continue walking child nodes. */
    NCC_XFORM_SKIP_CHILDREN, /**< Skip child traversal for this node. */
} ncc_xform_control_t;

// ============================================================================
// Transform function signatures
// ============================================================================

/**
 * @brief Post-order transform: called after children are processed.
 * @param ctx   Transform context.
 * @param node  Current tree node.
 * @return Replacement node, or nullptr for no change.
 */
typedef ncc_parse_tree_t *(*ncc_xform_fn_t)(ncc_xform_ctx_t  *ctx,
                                                ncc_parse_tree_t *node);

/**
 * @brief Pre-order transform: called before children are processed.
 * @param ctx      Transform context.
 * @param node     Current tree node.
 * @param control  Output: set to SKIP_CHILDREN to skip child walk.
 * @return Replacement node, or nullptr for no change.
 */
typedef ncc_parse_tree_t *(*ncc_xform_pre_fn_t)(ncc_xform_ctx_t    *ctx,
                                                    ncc_parse_tree_t   *node,
                                                    ncc_xform_control_t *control);

// ============================================================================
// Transform entry (one registered transform)
// ============================================================================

/** @brief A single registered transform (pre-order or post-order). */
typedef struct {
    union {
        ncc_xform_fn_t     post_fn; /**< Post-order function. */
        ncc_xform_pre_fn_t pre_fn;  /**< Pre-order function. */
    };
    const char *name;  /**< Human-readable name for debugging. */
    bool        is_pre; /**< True if pre-order. */
} ncc_xform_entry_t;

// ============================================================================
// Registry
// ============================================================================

/** @brief Maximum number of NT slots for direct-indexed dispatch. */
#define NCC_XFORM_MAX_DIRECT_NTS 512

/** @brief Registry of tree transforms indexed by non-terminal. */
struct ncc_xform_registry_t {
    ncc_xform_entry_t **pre_order[NCC_XFORM_MAX_DIRECT_NTS];
    int                  pre_count[NCC_XFORM_MAX_DIRECT_NTS];
    int                  pre_cap[NCC_XFORM_MAX_DIRECT_NTS];

    ncc_xform_entry_t **post_order[NCC_XFORM_MAX_DIRECT_NTS];
    int                  post_count[NCC_XFORM_MAX_DIRECT_NTS];
    int                  post_cap[NCC_XFORM_MAX_DIRECT_NTS];

    ncc_xform_entry_t **wildcard_pre;
    int                  wildcard_pre_count;
    int                  wildcard_pre_cap;

    ncc_xform_entry_t **wildcard_post;
    int                  wildcard_post_count;
    int                  wildcard_post_cap;

    ncc_grammar_t      *grammar;
};

// ============================================================================
// Transform context
// ============================================================================

/** @brief Mutable context threaded through a transform pass. */
struct ncc_xform_ctx_t {
    ncc_grammar_t        *grammar;
    ncc_xform_registry_t *registry;
    ncc_parse_tree_t     *root;
    int                    depth;
    int                    pass;
    int                    nodes_visited;
    int                    nodes_replaced;
    int                    unique_id;
    void                  *user_data;
};

// ============================================================================
// Registry API
// ============================================================================

/**
 * @brief Initialize a transform registry.
 * @param reg      Registry to initialize.
 * @param grammar  Grammar for NT name resolution.
 */
void ncc_xform_registry_init(ncc_xform_registry_t *reg,
                                ncc_grammar_t        *grammar);

/** @brief Free all resources held by a registry. */
void ncc_xform_registry_free(ncc_xform_registry_t *reg);

/**
 * @brief Register a post-order transform for a non-terminal.
 *
 * Pass @p nt_name = nullptr to register a wildcard that matches all nodes.
 *
 * @return True on success, false if the NT name was not found.
 */
bool ncc_xform_register(ncc_xform_registry_t *reg,
                           const char            *nt_name,
                           ncc_xform_fn_t        fn,
                           const char            *name);

/**
 * @brief Register a pre-order transform for a non-terminal.
 *
 * Pass @p nt_name = nullptr for a wildcard.
 */
bool ncc_xform_register_pre(ncc_xform_registry_t *reg,
                               const char            *nt_name,
                               ncc_xform_pre_fn_t    fn,
                               const char            *name);

/** @brief Register a post-order transform by NT id. */
bool ncc_xform_register_by_id(ncc_xform_registry_t *reg,
                                 int64_t                nt_id,
                                 ncc_xform_fn_t        fn,
                                 const char            *name);

/** @brief Register a pre-order transform by NT id. */
bool ncc_xform_register_pre_by_id(ncc_xform_registry_t *reg,
                                     int64_t                nt_id,
                                     ncc_xform_pre_fn_t    fn,
                                     const char            *name);

// ============================================================================
// Context API
// ============================================================================

/**
 * @brief Initialize a transform context.
 * @param ctx      Context to initialize.
 * @param grammar  Grammar for NT lookups.
 * @param reg      Transform registry.
 * @param root     Root tree node.
 */
void ncc_xform_ctx_init(ncc_xform_ctx_t      *ctx,
                           ncc_grammar_t         *grammar,
                           ncc_xform_registry_t  *reg,
                           ncc_parse_tree_t      *root);

// ============================================================================
// Transform application
// ============================================================================

/**
 * @brief Apply one pass of transforms over the tree.
 * @return Transformed root tree (may differ from original).
 */
ncc_parse_tree_t *ncc_xform_apply(ncc_xform_registry_t *reg,
                                      ncc_xform_ctx_t      *ctx);

/**
 * @brief Apply transforms in multiple passes until fixpoint or limit.
 * @param max_passes  Maximum number of passes (0 defaults to 100).
 * @return Transformed root tree.
 */
ncc_parse_tree_t *ncc_xform_apply_multi(ncc_xform_registry_t *reg,
                                            ncc_xform_ctx_t      *ctx,
                                            int                    max_passes);

// ============================================================================
// Tree mutation primitives
// ============================================================================

/**
 * @brief Replace child at index.
 * @return True on success, false if index out of range.
 */
bool ncc_xform_set_child(ncc_parse_tree_t *parent, size_t index,
                            ncc_parse_tree_t *new_child);

/**
 * @brief Remove child at index, compacting the array.
 * @return The removed child, or nullptr if out of range.
 */
ncc_parse_tree_t *ncc_xform_remove_child(ncc_parse_tree_t *parent,
                                              size_t             index);

/**
 * @brief Insert a child at index, shifting others right.
 * @return True on success, false if out of range.
 */
bool ncc_xform_insert_child(ncc_parse_tree_t *parent, size_t index,
                               ncc_parse_tree_t *child);

/**
 * @brief Deep-copy a subtree.
 * @return A new independent copy.
 */
ncc_parse_tree_t *ncc_xform_clone(ncc_parse_tree_t *tree);

// ============================================================================
// Node construction helpers
// ============================================================================

/**
 * @brief Create a non-terminal node with no children.
 * @param grammar     Grammar for NT id resolution.
 * @param nt_name     Non-terminal name (borrowed, copied into ncc_string_t).
 * @param rule_index  Index of the production rule.
 */
ncc_parse_tree_t *ncc_xform_make_nt_node(ncc_grammar_t *grammar,
                                              const char     *nt_name,
                                              int32_t         rule_index);

/**
 * @brief Create a terminal/token leaf node.
 * @param tid    Terminal ID.
 * @param value  Token text (copied into string).
 * @param line   Source line number.
 * @param col    Source column number.
 */
ncc_parse_tree_t *ncc_xform_make_token_node(int64_t     tid,
                                                const char *value,
                                                uint32_t    line,
                                                uint32_t    col);

/**
 * @brief Create a non-terminal with pre-populated children.
 * @param grammar     Grammar for NT id resolution.
 * @param nt_name     Non-terminal name.
 * @param rule_index  Rule index.
 * @param children    Array of child tree nodes.
 * @param count       Number of children.
 */
ncc_parse_tree_t *ncc_xform_make_node_with_children(
    ncc_grammar_t     *grammar,
    const char         *nt_name,
    int32_t             rule_index,
    ncc_parse_tree_t **children,
    size_t              count);

// ============================================================================
// Template-based subtree construction
// ============================================================================

/** @brief Error: no tokenizer callback available. */
#define NCC_XFORM_ERR_NO_TOKENIZER 1
/** @brief Error: @p nt_name does not name a non-terminal in the grammar. */
#define NCC_XFORM_ERR_UNKNOWN_NT   2
/** @brief Error: tokenization or parsing of the source string failed. */
#define NCC_XFORM_ERR_PARSE_FAILED 3

/**
 * @brief Parse a source string into a subtree using a specific start symbol.
 *
 * Tokenizes @p source and parses the token stream against @p grammar
 * starting from the non-terminal named @p nt_name.
 *
 * The tokenizer callback is stored in the grammar on first use. Pass
 * a non-nullptr @p tokenize to override it; otherwise the previously
 * stored callback is reused.
 *
 * @param grammar   Grammar to parse with (must be finalized).
 * @param nt_name   Non-terminal to use as start symbol.
 * @param source    Null-terminated source text to parse.
 * @param tokenize  Tokenizer callback, or nullptr to reuse grammar's stored one.
 * @return Ok with cloned parse tree, or err on failure.
 */
ncc_result_t(ncc_parse_tree_ptr_t)
ncc_xform_parse_template(ncc_grammar_t *grammar,
                           const char     *nt_name,
                           const char     *source,
                           ncc_scan_cb_t  tokenize);

// ============================================================================
// Parent pointer repair
// ============================================================================

/**
 * @brief Recursively set parent pointers on all children of @p tree.
 *
 * After cloning or splicing subtrees, parent pointers may be stale.
 * Call this on the subtree root to repair them.
 */
void ncc_xform_set_parent_pointers(ncc_parse_tree_t *tree);

// ============================================================================
// Parent pointer navigation
// ============================================================================

/**
 * @brief Walk up the parent chain to find the nearest ancestor with
 *        the given NT name.
 * @param node     Starting node.
 * @param nt_name  Non-terminal name to search for.
 * @return The ancestor node, or nullptr if not found.
 */
ncc_parse_tree_t *ncc_xform_find_ancestor(ncc_parse_tree_t *node,
                                              const char        *nt_name);
