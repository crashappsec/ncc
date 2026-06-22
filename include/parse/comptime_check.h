#pragma once

#include "parse/bnf.h"
#include "parse/parse_tree.h"
#include "parse/symtab.h"

/**
 * Diagnose illegal writes to WP-002 `[[n00b::comptime]]` variables and
 * validate WP-004 `[[n00b::optional]]`.
 *
 * The pass enforces D-005's frozen-output rule for direct comptime variable
 * mutations and syntactically rooted member/subscript mutations. Mutations are
 * allowed only inside `comptime_main`. It also enforces the D-007 optional
 * rule: `[[n00b::optional]]` is valid only on a present `comptime_main`
 * function definition.
 *
 * @pre @p tree is the pre-transform translation unit, parent pointers have
 *      been set, and @p symtab was populated from that same tree.
 * @post diagnostics for illegal comptime writes or optional misuse have been
 *       emitted to stderr; @p has_optional_out is true iff a valid optional
 *       attribute appeared on `comptime_main`; valid optional attributes are
 *       stripped before clang.
 * @return number of hard errors emitted to stderr.
 */
int ncc_comptime_check(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                       ncc_symtab_t *symtab, bool *has_optional_out);
