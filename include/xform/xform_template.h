#pragma once

/**
 * @file xform_template.h
 * @brief Parameterized code template engine for ncc transforms.
 *
 * Templates are C code strings with `$N` substitution slots.  At
 * registration time, the fixed portions are pre-lexed into token arrays.
 * At instantiation time, only the dynamic `$N` argument strings are lexed,
 * then spliced into the pre-lexed token runs and parsed as a single stream.
 *
 * ### Usage
 *
 * ```c
 * ncc_template_registry_t reg;
 * ncc_template_registry_init(&reg, grammar, tokenize_cb);
 * ncc_template_register(&reg, "my_tmpl", "primary_expression",
 *                        "($0 + $1)");
 *
 * const char *args[] = { "42", "x" };
 * ncc_result_t(ncc_parse_tree_ptr_t) r =
 *     ncc_template_instantiate(&reg, "my_tmpl", args, 2);
 * // ...
 * ncc_template_registry_free(&reg);
 * ```
 */

#include "xform/transform.h"
#include "lib/dict.h"

// ============================================================================
// Error codes
// ============================================================================

/** @brief Template name not found in registry. */
#define NCC_TMPL_ERR_NOT_FOUND     10
/** @brief Argument count does not match template slot count. */
#define NCC_TMPL_ERR_ARG_COUNT     11
/** @brief Lexing a `$N` argument string failed. */
#define NCC_TMPL_ERR_ARG_LEX       12
/** @brief Parsing the assembled token stream failed. */
#define NCC_TMPL_ERR_PARSE_FAILED  13

// ============================================================================
// Types
// ============================================================================

/**
 * @brief One segment of a compiled template.
 *
 * Contains pre-lexed fixed tokens followed by a `$N` substitution
 * slot (or `slot_index == -1` for trailing text with no substitution).
 */
typedef struct {
    ncc_token_info_t **tokens;
    int32_t             count;
    int32_t             slot_index; /**< Slot number, or -1 if none. */
} ncc_template_segment_t;

/** @brief A compiled template ready for instantiation. */
typedef struct {
    ncc_string_t             name;
    ncc_string_t             start_symbol;
    ncc_template_segment_t *segments;
    int32_t                  num_segments;
    int32_t                  num_slots; /**< max($N) + 1 */
} ncc_compiled_template_t;

/** @brief Registry caching compiled templates by name. */
typedef struct {
    ncc_dict_t     templates; /**< name -> ncc_compiled_template_t* */
    ncc_grammar_t *grammar;
    ncc_scan_cb_t  tokenize;
} ncc_template_registry_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize a template registry.
 * @param reg       Registry to initialize.
 * @param grammar   Grammar for parsing (must be finalized).
 * @param tokenize  Tokenizer callback for lexing template fragments.
 */
void ncc_template_registry_init(ncc_template_registry_t *reg,
                                 ncc_grammar_t           *grammar,
                                 ncc_scan_cb_t            tokenize);

/** @brief Free all resources held by the registry. */
void ncc_template_registry_free(ncc_template_registry_t *reg);

/**
 * @brief Compile and cache a template.
 *
 * Scans @p template_text for `$N` substitution slots, pre-lexes the
 * fixed text between slots, and stores the compiled result keyed by
 * @p name.
 *
 * @param reg            Registry to add the template to.
 * @param name           Unique template name.
 * @param start_symbol   Grammar non-terminal to parse as.
 * @param template_text  Template string with `$N` placeholders.
 * @return True on success, false if fixed-segment lexing fails.
 */
bool ncc_template_register(ncc_template_registry_t *reg,
                            const char               *name,
                            const char               *start_symbol,
                            const char               *template_text);

/**
 * @brief Instantiate a registered template with argument strings.
 *
 * Each `$N` slot is replaced by the corresponding entry in @p args.
 * The assembled token stream is parsed against the template's start
 * symbol and the resulting subtree is returned.
 *
 * @param reg    Registry containing the template.
 * @param name   Template name (must have been registered).
 * @param args   Array of C strings, one per slot.
 * @param nargs  Number of arguments (must equal template's num_slots).
 * @return Ok with cloned parse tree, or err with error code.
 */
ncc_result_t(ncc_parse_tree_ptr_t)
ncc_template_instantiate(ncc_template_registry_t *reg,
                          const char               *name,
                          const char              **args,
                          int                       nargs);

/**
 * @brief Query the number of substitution slots in a registered template.
 *
 * @param reg   Registry containing the template.
 * @param name  Template name.
 * @return Slot count (>= 0), or -1 if not found.
 */
int ncc_template_slot_count(ncc_template_registry_t *reg,
                             const char               *name);
