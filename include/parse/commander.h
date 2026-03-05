#pragma once

/**
 * @file commander.h
 * @brief Grammar-based command-line parser.
 *
 * Commander builds a formal grammar from your command-line specification,
 * then parses argv using PWZ. This gives you unambiguous, predictable
 * parsing with proper error reporting.
 *
 * Usage:
 *   ncc_cmdr_t *c = ncc_cmdr_new();
 *   ncc_cmdr_add_flag(c, nullptr, "--verbose", NCC_CMDR_TYPE_BOOL, false, nullptr);
 *   ncc_cmdr_add_positional(c, nullptr, "file", NCC_CMDR_TYPE_WORD, 1, -1);
 *   ncc_cmdr_result_t *r = ncc_cmdr_parse(c, argc - 1, argv + 1);
 *   if (r->ok) { ... }
 *   ncc_cmdr_result_free(r);
 *   ncc_cmdr_free(c);
 */

#include "parse/grammar.h"
#include "parse/parse_tree.h"
#include "lib/dict.h"
#include "lib/string.h"

#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Token type indices
// ============================================================================

enum {
    NCC_CMDR_TID_WORD  = 0,
    NCC_CMDR_TID_INT   = 1,
    NCC_CMDR_TID_FLOAT = 2,
    NCC_CMDR_TID_BOOL  = 3,
    NCC_CMDR_TID_EQ    = 4,
    NCC_CMDR_TID_COMMA = 5,
    NCC_CMDR_TID_DD    = 6,
    NCC_CMDR_TID_FLAG  = 7,
    NCC_CMDR_TID_COUNT = 8,
};

// ============================================================================
// Types
// ============================================================================

typedef enum {
    NCC_CMDR_TYPE_BOOL,
    NCC_CMDR_TYPE_WORD,
    NCC_CMDR_TYPE_INT,
    NCC_CMDR_TYPE_FLOAT,
} ncc_cmdr_arg_type_t;

typedef enum {
    NCC_CMDR_VAL_NONE,
    NCC_CMDR_VAL_BOOL,
    NCC_CMDR_VAL_INT,
    NCC_CMDR_VAL_FLOAT,
    NCC_CMDR_VAL_STR,
} ncc_cmdr_val_tag_t;

typedef struct {
    ncc_cmdr_val_tag_t tag;
    union {
        bool         b;
        int64_t      i;
        double       f;
        ncc_string_t s;
    };
} ncc_cmdr_val_t;

typedef struct {
    ncc_string_t        value;
    int64_t             int_val;
    double              float_val;
} ncc_cmdr_arg_t;

typedef struct {
    ncc_string_t         name;
    ncc_string_t         short_name;
    ncc_cmdr_arg_type_t  value_type;
    bool                 takes_value;
    ncc_string_t         doc;
    int64_t              terminal_id;
} ncc_cmdr_flag_spec_t;

typedef struct {
    ncc_string_t         name;
    ncc_cmdr_arg_type_t  type;
    int                  min;
    int                  max;
} ncc_cmdr_positional_spec_t;

typedef struct ncc_cmdr_command {
    ncc_string_t                 name;
    ncc_string_t                 doc;
    ncc_cmdr_flag_spec_t        *flags;
    int32_t                      n_flags;
    int32_t                      flags_cap;
    ncc_cmdr_positional_spec_t  *positionals;
    int32_t                      n_positionals;
    int32_t                      positionals_cap;
    struct ncc_cmdr_command     *subcommands;
    int32_t                      n_subcommands;
    int32_t                      subcommands_cap;
} ncc_cmdr_command_t;

typedef struct {
    ncc_grammar_t      *grammar;
    ncc_cmdr_command_t  root;
    int64_t              next_flag_id;
    bool                 finalized;
    ncc_string_t         name;
    int64_t              tok_ids[NCC_CMDR_TID_COUNT];
} ncc_cmdr_t;

typedef struct {
    ncc_string_t         command;
    ncc_dict_t           flags;
    ncc_cmdr_arg_t      *args;
    int32_t              n_args;
    ncc_string_t        *errors;
    int32_t              n_errors;
    bool                 ok;
    ncc_parse_tree_t    *tree;
} ncc_cmdr_result_t;

// ============================================================================
// Lifecycle
// ============================================================================

ncc_cmdr_t *ncc_cmdr_new(void);
void         ncc_cmdr_free(ncc_cmdr_t *c);

// ============================================================================
// Builder API
// ============================================================================

void ncc_cmdr_set_name(ncc_cmdr_t *c, const char *name);
void ncc_cmdr_add_command(ncc_cmdr_t *c, const char *name, const char *doc);
void ncc_cmdr_add_subcommand(ncc_cmdr_t *c, const char *parent,
                               const char *name, const char *doc);
void ncc_cmdr_add_flag(ncc_cmdr_t *c, const char *command,
                          const char *flag_name, ncc_cmdr_arg_type_t type,
                          bool takes_value, const char *doc);
void ncc_cmdr_add_flag_alias(ncc_cmdr_t *c, const char *command,
                                const char *flag_name, const char *alias);
void ncc_cmdr_add_positional(ncc_cmdr_t *c, const char *command,
                                const char *name, ncc_cmdr_arg_type_t type,
                                int min, int max);
void ncc_cmdr_finalize(ncc_cmdr_t *c);

// ============================================================================
// Parsing
// ============================================================================

ncc_cmdr_result_t *ncc_cmdr_parse(ncc_cmdr_t *c, int argc,
                                      const char **argv);
ncc_cmdr_result_t *ncc_cmdr_parse_string(ncc_cmdr_t *c,
                                              const char *cmdline);
void                ncc_cmdr_result_free(ncc_cmdr_result_t *r);

// ============================================================================
// Result queries
// ============================================================================

const char *ncc_cmdr_result_command(ncc_cmdr_result_t *r);
bool        ncc_cmdr_flag_present(ncc_cmdr_result_t *r, const char *flag);
ncc_cmdr_val_t *ncc_cmdr_flag_get(ncc_cmdr_result_t *r, const char *flag);
const char *ncc_cmdr_flag_str(ncc_cmdr_result_t *r, const char *flag);
int64_t     ncc_cmdr_flag_int(ncc_cmdr_result_t *r, const char *flag);
bool        ncc_cmdr_flag_bool(ncc_cmdr_result_t *r, const char *flag);
int32_t     ncc_cmdr_arg_count(ncc_cmdr_result_t *r);
const char *ncc_cmdr_arg_str(ncc_cmdr_result_t *r, int index);
int64_t     ncc_cmdr_arg_int(ncc_cmdr_result_t *r, int index);

// ============================================================================
// Error output
// ============================================================================

int32_t ncc_cmdr_print_errors(ncc_cmdr_result_t *r, FILE *out);
