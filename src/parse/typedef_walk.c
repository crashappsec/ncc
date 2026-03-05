// typedef_walk.c - Hardcoded post-parse typedef reclassification.
//
// DFS post-order walk that finds <declaration> nodes containing
// "typedef" in their <declaration_specifiers> subtree, extracts
// declared names from <init_declarator_list>, and reclassifies
// matching IDENTIFIER tokens to TYPEDEF_NAME in subsequent code.
//
// Scope-aware: <compound_statement> and <function_definition>
// limit reclassification to their token range.

#include "parse/typedef_walk.h"
#include "internal/parse/grammar_internal.h"
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static int64_t
find_identifier_tid(ncc_grammar_t *g)
{
    ncc_string_t name  = NCC_STRING_STATIC("IDENTIFIER");
    bool         found = false;
    void        *val   = _ncc_dict_get(g->terminal_map,
                                       (void *)name.data, &found);
    if (found) {
        return (int64_t)(intptr_t)val;
    }

    return NCC_TOK_IDENTIFIER;
}

static bool
subtree_contains_token_text(ncc_parse_tree_t *node, ncc_string_t text)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);
            if (ncc_string_eq(val, text)) {
                return true;
            }
        }
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_contains_token_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

static ncc_token_info_t *
extract_first_identifier(ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && tok->tid == (int32_t)id_tid) {
            return tok;
        }
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *found = extract_first_identifier(
            ncc_tree_child(node, i), id_tid);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static int32_t
reclassify_tokens(ncc_token_info_t **tokens, int32_t ntokens,
                  int32_t after_index, int32_t scope_end,
                  ncc_string_t name,
                  int64_t id_tid, int64_t new_tid)
{
    int32_t count = 0;
    int32_t limit = (scope_end >= 0 && scope_end < ntokens)
                        ? scope_end
                        : ntokens;

    for (int32_t i = after_index + 1; i < limit; i++) {
        ncc_token_info_t *tok = tokens[i];

        if (!tok || tok->tid != (int32_t)id_tid) {
            continue;
        }

        if (!ncc_option_is_set(tok->value)) {
            continue;
        }

        ncc_string_t val = ncc_option_get(tok->value);
        if (ncc_string_eq(val, name)) {
            tok->tid = (int32_t)new_tid;
            count++;
        }
    }

    return count;
}

// ============================================================================
// NT name checks
// ============================================================================

static inline bool
name_eq(ncc_string_t s, const char *lit)
{
    if (!s.data || !lit) {
        return false;
    }
    return strcmp(s.data, lit) == 0;
}

static inline bool
is_scope_nt(ncc_string_t name)
{
    return name_eq(name, "compound_statement")
        || name_eq(name, "function_definition");
}

static inline bool
is_declaration_nt(ncc_string_t name)
{
    return name_eq(name, "declaration");
}

static ncc_parse_tree_t *
find_child_by_name(ncc_parse_tree_t *node, const char *child_name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (!child || ncc_tree_is_leaf(child)) {
            continue;
        }

        ncc_nt_node_t *cpn = &ncc_tree_node_value(child);

        if (cpn->group_top) {
            // Recurse through group nodes.
            ncc_parse_tree_t *found = find_child_by_name(child, child_name);
            if (found) {
                return found;
            }
            continue;
        }

        if (name_eq(cpn->name, child_name)) {
            return child;
        }
    }

    return nullptr;
}

// ============================================================================
// DFS walk
// ============================================================================

static int32_t
typedef_walk_node(ncc_grammar_t     *g,
                  ncc_parse_tree_t  *node,
                  ncc_token_info_t **tokens,
                  int32_t            ntokens,
                  int64_t            id_tid,
                  int32_t            scope_end)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return 0;
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);

    // Recurse through group nodes transparently.
    if (pn->group_top) {
        int32_t count = 0;
        size_t  nc    = ncc_tree_num_children(node);
        for (size_t i = 0; i < nc; i++) {
            count += typedef_walk_node(g, ncc_tree_child(node, i),
                                       tokens, ntokens, id_tid, scope_end);
        }
        return count;
    }

    // Scope boundaries limit reclassification range.
    int32_t child_scope_end = scope_end;
    if (is_scope_nt(pn->name)) {
        child_scope_end = pn->end;
    }

    int32_t count = 0;

    // Post-order: recurse into children first.
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += typedef_walk_node(g, ncc_tree_child(node, i),
                                   tokens, ntokens, id_tid, child_scope_end);
    }

    // Check if this is a <declaration> node.
    if (!is_declaration_nt(pn->name)) {
        return count;
    }

    // Check if <declaration_specifiers> contains "typedef".
    ncc_parse_tree_t *decl_specs = find_child_by_name(
        node, "declaration_specifiers");
    if (!decl_specs) {
        return count;
    }

    ncc_string_t typedef_kw = NCC_STRING_STATIC("typedef");
    if (!subtree_contains_token_text(decl_specs, typedef_kw)) {
        return count;
    }

    // Extract identifiers from <init_declarator_list>.
    ncc_parse_tree_t *init_decl_list = find_child_by_name(
        node, "init_declarator_list");
    if (!init_decl_list) {
        return count;
    }

    // Walk children of init_declarator_list to find each declarator's
    // identifier.  Each child is an <init_declarator> or group wrapper.
    size_t lnc = ncc_tree_num_children(init_decl_list);
    for (size_t i = 0; i < lnc; i++) {
        ncc_parse_tree_t *item = ncc_tree_child(init_decl_list, i);
        ncc_token_info_t *id_tok = extract_first_identifier(item, id_tid);

        if (id_tok && ncc_option_is_set(id_tok->value)) {
            ncc_string_t name = ncc_option_get(id_tok->value);
            count += reclassify_tokens(tokens, ntokens, pn->end,
                                       child_scope_end, name, id_tid,
                                       NCC_TOK_TYPEDEF_NAME);
        }
    }

    return count;
}

// ============================================================================
// Public API
// ============================================================================

int32_t
ncc_typedef_walk(ncc_grammar_t    *g,
                 ncc_parse_tree_t *tree,
                 ncc_token_info_t **tokens,
                 int32_t            ntokens)
{
    if (!g || !tree || !tokens || ntokens <= 0) {
        return 0;
    }

    int64_t id_tid = find_identifier_tid(g);
    return typedef_walk_node(g, tree, tokens, ntokens, id_tid, -1);
}
