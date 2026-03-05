#pragma once

/**
 * @file token.h
 * @brief Token types, trivia, and token list for the ncc parser system.
 *
 * Tokens are the universal currency — every tokenizer produces them,
 * every parser consumes them, parse trees reference them.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/array.h"
#include "lib/list.h"
#include "lib/string.h"

// ============================================================================
// Constants
// ============================================================================

/** @brief Base token ID offset for user-registered terminals. */
#define NCC_TOK_START_ID   0x40000000

/** @brief Token ID for unrecognized input. */
#define NCC_TOK_OTHER      (-3)

/** @brief Token ID for whitespace/comments that should be skipped. */
#define NCC_TOK_IGNORED    (-2)

/** @brief Token ID indicating end of input. */
#define NCC_TOK_EOF        (-1)

/** @brief Default token ID for identifiers (first registered terminal). */
#define NCC_TOK_IDENTIFIER   (NCC_TOK_START_ID + 1)

/** @brief Token ID for typedef/type names (reclassified identifiers). */
#define NCC_TOK_TYPEDEF_NAME (NCC_TOK_START_ID + 2)

/** @brief Token ID for integer literals. */
#define NCC_TOK_INTEGER      (NCC_TOK_START_ID + 3)

/** @brief Token ID for floating-point literals. */
#define NCC_TOK_FLOAT        (NCC_TOK_START_ID + 4)

/** @brief Token ID for character literals. */
#define NCC_TOK_CHAR_LIT     (NCC_TOK_START_ID + 5)

/** @brief Token ID for string literals. */
#define NCC_TOK_STRING_LIT   (NCC_TOK_START_ID + 6)

// ============================================================================
// Trivia
// ============================================================================

/** @brief A piece of trivia (whitespace, comment) attached to a token. */
typedef struct ncc_trivia_t {
    ncc_string_t         text; /**< Trivia text (GC-managed data). */
    struct ncc_trivia_t *next; /**< Next trivia piece in the linked list. */
} ncc_trivia_t;

// ============================================================================
// Token
// ============================================================================

ncc_option_decl(ncc_string_t);

/** @brief Token with position, value, trivia, and user data. */
typedef struct ncc_token_info_t {
    void                          *user_info;       /**< User-defined data. */
    ncc_option_t(ncc_string_t)   value;           /**< Token text (optional). */
    ncc_option_t(ncc_string_t)   file;            /**< Source file path (optional). */
    ncc_trivia_t                 *leading_trivia;  /**< Whitespace/comments before token. */
    ncc_trivia_t                 *trailing_trivia; /**< Line comment after token (same line). */
    int32_t                        tid;             /**< Terminal ID assigned by tokenizer. */
    int32_t                        index;           /**< Index in token array. */
    uint32_t                       line;            /**< 1-based source line. */
    uint32_t                       column;          /**< 1-based source column. */
    uint32_t                       endcol;          /**< 1-based end column. */
    bool                           system_header;   /**< True if from system header. */
} ncc_token_info_t;

// ============================================================================
// Token list
// ============================================================================

ncc_list_decl(ncc_token_info_t);

typedef ncc_token_info_t *ncc_token_info_ptr_t;
ncc_array_decl(ncc_token_info_ptr_t);
ncc_list_decl(ncc_token_info_ptr_t);

/**
 * @brief Build a pointer array from a token list (for parser consumption).
 *
 * @param tl    Token list.
 * @param out   Receives pointer to array of `ncc_token_info_t *`.
 * @return      Number of tokens.
 */
extern int32_t ncc_token_list_build_ptrs(ncc_list_t(ncc_token_info_t) *tl,
                                           ncc_token_info_ptr_t **out);

// ============================================================================
// Tokenizer callback
// ============================================================================

// Forward declaration — parser type defined later.
typedef struct ncc_parser_t ncc_parser_t;

/** @brief Tokenizer callback that returns the next token ID. */
typedef int64_t (*ncc_tokenizer_fn)(ncc_parser_t *, void **);
