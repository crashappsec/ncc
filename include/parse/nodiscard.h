#pragma once

/**
 * @file nodiscard.h
 * @brief Must-check result returns ([[nodiscard]]-style, for result types).
 *
 * A function whose return type carries the result protocol (an aggregate with
 * both an `is_ok` and an `err` member — i.e. `n00b_result_t(T)`) advertises a
 * value the caller is expected to inspect. Calling such a function as a bare
 * expression statement (`f(x);`) discards the result, silently dropping the
 * error — the classic "forgot to check the return" bug.
 *
 * This pass warns on that. The opt-out is an explicit void cast (`(void)f(x);`),
 * matching real `nodiscard`. It is read-only: it emits diagnostics and never
 * transforms the tree. Run it after transforms so `_generic_struct` result
 * types are lowered to concrete tags resolvable through the symbol table (and
 * `_try`, which consumes a result, is already lowered away).
 */

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

/**
 * @brief Warn for each call whose result-protocol return value is discarded.
 *        Returns the number of warnings emitted.
 */
int ncc_nodiscard_check(ncc_grammar_t *g, ncc_parse_tree_t *tu,
                        ncc_symtab_t *symtab);
