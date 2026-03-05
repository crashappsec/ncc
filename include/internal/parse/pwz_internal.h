#pragma once

/**
 * @file pwz_internal.h
 * @internal
 * @brief Private types for the PWZ parser engine.
 */

#include "parse/pwz.h"
#include "scanner/token_stream.h"

// ============================================================================
// PWZ expression graph (built once from grammar)
// ============================================================================

typedef enum {
    PWZ_TOK,
    PWZ_SEQ,
    PWZ_ALT,
    PWZ_CLASS,
    PWZ_ANY,
} pwz_exp_kind_t;

typedef struct pwz_mem_t pwz_mem_t;
typedef struct pwz_exp_t pwz_exp_t;

typedef pwz_exp_t *pwz_exp_ptr_t;

// Must be declared before pwz_exp_t so the type is complete inside the union.
ncc_list_decl(pwz_exp_ptr_t);

struct pwz_exp_t {
    pwz_mem_t     *mem;
    pwz_exp_kind_t kind;
    union {
        struct {
            int64_t tid;
        } tok;
        struct {
            const char     *name;
            int64_t         nt_id;
            int32_t         rule_ix;
            pwz_exp_ptr_t  *children;  // ncc_alloc_array, fixed at creation
            int32_t         nchildren;
        } seq;
        struct {
            int64_t                    nt_id;
            ncc_list_t(pwz_exp_ptr_t) alts;  // growable via ncc_list_push
        } alt;
        struct {
            ncc_char_class_t cc;
        } cls;
    };
};

// ============================================================================
// Memo records (per-parse, GC-managed)
// ============================================================================

#define PWZ_POS_BOTTOM (-1)

typedef struct pwz_cxt_t pwz_cxt_t;

typedef struct pwz_cxt_node_t {
    pwz_cxt_t             *cxt;
    struct pwz_cxt_node_t *next;
} pwz_cxt_node_t;

struct pwz_mem_t {
    int32_t         start_pos;
    int32_t         end_pos;
    pwz_cxt_node_t *parents;
    pwz_exp_t      *result;
    bool            in_progress;
};

// ============================================================================
// Contexts (per-parse, GC-managed)
// ============================================================================

typedef enum {
    PWZ_CXT_TOP,
    PWZ_CXT_SEQ,
    PWZ_CXT_ALT,
} pwz_cxt_kind_t;

typedef struct pwz_cxt_t {
    pwz_cxt_kind_t kind;
    union {
        struct {
            pwz_mem_t     *mem;
            const char    *name;
            int64_t        nt_id;
            int32_t        rule_ix;
            pwz_exp_ptr_t *left;   // ncc_alloc_array, fixed at creation
            int32_t        nleft;
            pwz_exp_ptr_t *right;  // pointer into grammar exp children
            int32_t        nright;
        } seq;
        struct {
            pwz_mem_t *mem;
        } alt;
    };
} pwz_cxt_t;

// ============================================================================
// Zippers & worklist
// ============================================================================

typedef struct {
    pwz_exp_t *result;
    pwz_mem_t *mem;
} pwz_zipper_t;

// Container declarations for ncc_list_t usage.
ncc_list_decl(pwz_zipper_t);

typedef ncc_parse_tree_t *ncc_parse_tree_ptr_t;
ncc_list_decl(ncc_parse_tree_ptr_t);

// ============================================================================
// Parser state (full definition)
// ============================================================================

struct ncc_pwz_parser_t {
    ncc_grammar_t              *grammar;
    pwz_exp_t                   *start_exp;
    pwz_exp_ptr_t               *nt_exps;     // ncc_alloc_array, indexed by NT id
    ncc_list_t(pwz_exp_ptr_t)   all_exps;    // all grammar exp nodes (for memo reset)

    // Per-parse state (GC-managed, cleared on reset)
    ncc_list_t(pwz_zipper_t)    worklist;
    ncc_list_t(pwz_zipper_t)    worklist_swap;
    ncc_list_t(pwz_exp_ptr_t)   tops;

    ncc_token_stream_t              *stream;

    ncc_parse_tree_t           *result_tree;
    ncc_parse_tree_array_t      result_trees;

    pwz_mem_t                   *mem_bottom;
    pwz_exp_t                   *exp_bottom;
};
