#pragma once

/**
 * @file pwz_internal.h
 * @internal
 * @brief Private types for the PWZ parser engine.
 */

#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/arena.h"

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct pwz_exp_t pwz_exp_t;
typedef struct pwz_cxt_t pwz_cxt_t;

typedef pwz_exp_t *pwz_exp_ptr_t;

// Must be declared before pwz_exp_t so the type is complete inside the union.
ncc_list_decl(pwz_exp_ptr_t);

// ============================================================================
// Memo records (inline in grammar exps, arena-allocated for seq_memos)
// ============================================================================

// Bit 31 of end_pos is used as the in_progress flag.
#define PWZ_MEM_IN_PROGRESS_BIT ((int32_t)(1u << 31))

// Sentinel for "no position yet". Must not have bit 31 set.
#define PWZ_POS_BOTTOM ((int32_t)0x7FFFFFFE)

typedef struct pwz_cxt_node_t {
    pwz_cxt_t             *cxt;
    struct pwz_cxt_node_t *next;
} pwz_cxt_node_t;

typedef struct pwz_mem_t {
    pwz_cxt_node_t *parents;
    pwz_exp_t      *result;
    int32_t         start_pos;
    int32_t         end_pos;    // bit 31 = in_progress flag
    uint16_t        refcount;
} pwz_mem_t;

static inline bool
pwz_mem_in_progress(const pwz_mem_t *m)
{
    return (m->end_pos & PWZ_MEM_IN_PROGRESS_BIT) != 0;
}

static inline void
pwz_mem_set_in_progress(pwz_mem_t *m, bool v)
{
    if (v) {
        m->end_pos |= PWZ_MEM_IN_PROGRESS_BIT;
    }
    else {
        m->end_pos &= ~PWZ_MEM_IN_PROGRESS_BIT;
    }
}

static inline int32_t
pwz_mem_end_pos(const pwz_mem_t *m)
{
    return m->end_pos & ~PWZ_MEM_IN_PROGRESS_BIT;
}

// ============================================================================
// Contexts (per-parse, arena-allocated)
// ============================================================================

typedef enum : uint8_t {
    PWZ_CXT_TOP,
    PWZ_CXT_SEQ,
    PWZ_CXT_ALT,
} pwz_cxt_kind_t;

typedef struct pwz_cxt_t {
    pwz_cxt_kind_t kind;
    uint8_t        _pad;
    int16_t        nt_id;
    int16_t        rule_ix;
    int16_t        nleft;
    union {
        struct {
            pwz_mem_t     *mem;        // parent memo to propagate completion to
            pwz_exp_ptr_t *left;
            pwz_exp_ptr_t *right;
            int16_t        nright;
        } seq;
        struct {
            pwz_mem_t *mem;
        } alt;
    };
} pwz_cxt_t;

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

struct pwz_exp_t {
    pwz_mem_t     *mem;          // Per-position memo (arena-allocated)
    union {
        int32_t tid;             // PWZ_TOK: terminal ID
        int16_t nt_id;           // PWZ_SEQ, PWZ_ALT: non-terminal ID
        ncc_char_class_t cc;     // PWZ_CLASS: character class
    };
    pwz_exp_kind_t kind;
    int16_t         rule_ix;     // PWZ_SEQ: rule index
    int16_t         nchildren;   // PWZ_SEQ: child count
    union {
        struct {
            const char     *name;
            pwz_exp_ptr_t  *children;
        } seq;
        struct {
            ncc_list_t(pwz_exp_ptr_t) alts;
        } alt;
    };
};

// ============================================================================
// Zippers & worklist
// ============================================================================

typedef struct {
    pwz_exp_t *result;
    pwz_mem_t *mem;
} pwz_zipper_t;

ncc_list_decl(pwz_zipper_t);

typedef ncc_parse_tree_t *ncc_parse_tree_ptr_t;
ncc_list_decl(ncc_parse_tree_ptr_t);

// ============================================================================
// Parser state (full definition)
// ============================================================================

struct ncc_pwz_parser_t {
    ncc_grammar_t              *grammar;
    pwz_exp_t                   *start_exp;
    pwz_exp_ptr_t               *nt_exps;
    ncc_list_t(pwz_exp_ptr_t)   all_exps;

    // Per-parse arena (result_exps, child arrays, memos, contexts, cxt_nodes).
    ncc_arena_t                 parse_arena;
    pwz_mem_t                  *free_mems;    // free list for recycled memos
    ncc_list_t(pwz_zipper_t)    worklist;
    ncc_list_t(pwz_zipper_t)    worklist_swap;
    ncc_list_t(pwz_exp_ptr_t)   tops;

    ncc_token_stream_t         *stream;

    ncc_parse_tree_t           *result_tree;
    ncc_parse_tree_array_t      result_trees;

    pwz_mem_t                   *mem_bottom;
    pwz_exp_t                   *exp_bottom;
};
