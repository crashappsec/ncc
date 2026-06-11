#pragma once

/**
 * @file nullability.h
 * @brief Flow-sensitive null-state analysis for `?`-qualified pointers.
 *
 * A `?T` value may be null. This pass walks each function body, tracking
 * whether each `?`-declared variable is currently NONNULL or NULLABLE, and
 * warns when a NULLABLE value is dereferenced (`*p`, `p->m`, `p[i]`) without a
 * dominating null check. Null checks (`if (p) …`, `if (!p) return;`) refine the
 * state. Reads the `?` markers from the parse tree before they are stripped.
 */

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

/**
 * @brief Run the null-state analysis over a translation unit, emitting
 *        `ncc: warning:` diagnostics for unchecked dereferences of nullable
 *        values. Returns the number of warnings emitted.
 */
int ncc_nullability_check(ncc_grammar_t *g, ncc_parse_tree_t *tu,
                          ncc_symtab_t *symtab);
