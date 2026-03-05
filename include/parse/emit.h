#pragma once

/**
 * @file emit.h
 * @brief Code emission (tree to formatted source) and BNF emission.
 *
 * Two-phase Wadler/Lindig approach:
 * Phase 1: Tree walk -> document command stream
 * Phase 2: Layout resolution -> formatted output
 *
 * Annotations on NTs control formatting: @indent, @group, @concat,
 * @blankline, @softline($N), @hardline($N), @nospace($N), @align($N).
 */

#include "parse/grammar.h"
#include "parse/parse_tree.h"
#include "lib/string.h"
#include <stdint.h>
#include <stdio.h>

// ============================================================================
// Document command types (public for advanced users)
// ============================================================================

typedef enum {
    NCC_DOC_TEXT,
    NCC_DOC_SPACE,
    NCC_DOC_SOFTLINE,
    NCC_DOC_HARDLINE,
    NCC_DOC_BLANKLINE,
    NCC_DOC_INDENT,
    NCC_DOC_DEDENT,
    NCC_DOC_GROUP_BEGIN,
    NCC_DOC_GROUP_END,
    NCC_DOC_ALIGN_BEGIN,
    NCC_DOC_ALIGN_END,
} ncc_doc_kind_t;

// ============================================================================
// Per-NT formatting rule (for style tables)
// ============================================================================

typedef struct {
    const char *nt_name;
    bool        indent;
    bool        group;
    bool        concat;
    bool        blankline_after;
    int32_t    *hardline_before;  // -1 terminated array
    int32_t    *softline_before;  // -1 terminated array
    int32_t    *nospace_before;   // -1 terminated array
    int32_t    *align_to;         // -1 terminated array
} ncc_pprint_rule_t;

typedef ncc_pprint_rule_t *ncc_pprint_style_t;

// ============================================================================
// Options
// ============================================================================

typedef enum {
    NCC_PPRINT_SPACES,
    NCC_PPRINT_TABS,
} ncc_indent_style_t;

typedef struct {
    int32_t              line_width;
    int32_t              indent_size;
    ncc_indent_style_t  indent_style;
    bool                 use_unicode_width;
    FILE                *out;
    const char          *newline;
    ncc_pprint_style_t  style;
} ncc_pprint_opts_t;

// ============================================================================
// Code pretty-printing (tree -> formatted source)
// ============================================================================

/**
 * @brief Pretty-print a parse tree into formatted source.
 * @param g     Grammar for NT name resolution and annotation lookup.
 * @param tree  Root of the parse tree.
 * @param opts  Pretty-printer options.
 * @return ncc_string_t with formatted source, or empty if opts.out was set.
 */
ncc_string_t ncc_pprint(ncc_grammar_t    *g,
                         ncc_parse_tree_t *tree,
                         ncc_pprint_opts_t opts);

// ============================================================================
// BNF emission (grammar -> BNF text, with annotations)
// ============================================================================

/**
 * @brief Emit a grammar as BNF text.
 * @param g     Grammar to emit.
 * @param opts  Pretty-printer options for formatting the output.
 * @return ncc_string_t with BNF text, or empty if opts.out was set.
 */
ncc_string_t ncc_grammar_emit_bnf(ncc_grammar_t    *g,
                                   ncc_pprint_opts_t opts);
