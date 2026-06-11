// symbol_populate.c — Build a scoped symbol table from a parsed TU.
//
// Scope-aware DFS over the parse tree. Records typedefs, variables, functions,
// parameters, struct/union/enum tags, and enum constants into an ncc_symtab_t,
// each with the parse subtree describing its type. Ordinary names go in the ""
// namespace; tags go in "tag". Scopes are pushed at function_definition and
// compound_statement boundaries so shadowing resolves correctly.
//
// This is deliberately a thin first layer: it records WHERE each name is
// declared and the syntactic type that declares it. Deriving a normalized type
// from (specifiers, declarator) and typing expressions is the job of the type
// model built on top of this table.

#include "parse/symbol_populate.h"
#include "internal/parse/grammar_internal.h"
#include <string.h>

// ============================================================================
// Tree helpers (parse-layer; mirror the primitives in typedef_walk.c)
// ============================================================================

static int64_t
identifier_tid(ncc_grammar_t *g)
{
    ncc_string_t name  = NCC_STRING_STATIC("IDENTIFIER");
    bool         found = false;
    void        *val   = _ncc_dict_get(g->terminal_map, (void *)name.data,
                                       &found);
    return found ? (int64_t)(intptr_t)val : NCC_TOK_IDENTIFIER;
}

static bool
nt_is(ncc_parse_tree_t *node, const char *lit)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    ncc_string_t name = ncc_tree_node_value(node).name;
    return name.data && strcmp(name.data, lit) == 0;
}

// Direct child by NT name, transparently descending through group wrappers
// (the same convention typedef_walk.c uses).
static ncc_parse_tree_t *
child_nt(ncc_parse_tree_t *node, const char *child_name)
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
            ncc_parse_tree_t *found = child_nt(child, child_name);
            if (found) {
                return found;
            }
            continue;
        }
        if (cpn->name.data && strcmp(cpn->name.data, child_name) == 0) {
            return child;
        }
    }
    return nullptr;
}

static bool
subtree_has_token(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);
            return val.data && strcmp(val.data, text) == 0;
        }
        return false;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_token(ncc_tree_child(node, i), text)) {
            return true;
        }
    }
    return false;
}

// First IDENTIFIER token in a declarator that names the declared entity — i.e.
// the first identifier NOT inside a parameter list (which would be a parameter
// name, not the name being declared). Handles `*x`, `x[]`, `(*fp)(args)`.
static ncc_token_info_t *
declared_name_tok(ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        return (tok && tok->tid == (int32_t)id_tid) ? tok : nullptr;
    }
    // Do not descend into parameter lists: their identifiers are parameter
    // names, not the name being declared here.
    if (nt_is(node, "parameter_type_list") || nt_is(node, "parameter_list")) {
        return nullptr;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *t = declared_name_tok(ncc_tree_child(node, i), id_tid);
        if (t) {
            return t;
        }
    }
    return nullptr;
}

static ncc_string_t
tok_text(ncc_token_info_t *tok)
{
    if (tok && ncc_option_is_set(tok->value)) {
        return ncc_option_get(tok->value);
    }
    return ncc_string_empty();
}

// ============================================================================
// Recording
// ============================================================================

// Namespace names. Returned from helpers because NCC_STRING_STATIC is a
// compound literal (not a valid file-scope static initializer).
static inline ncc_string_t
ns_ord(void)
{
    return NCC_STRING_STATIC("");
}
static inline ncc_string_t
ns_tag(void)
{
    return NCC_STRING_STATIC("tag");
}

static void scan_params(ncc_symtab_t *st, ncc_parse_tree_t *node,
                        int64_t id_tid);

// In-order leaf scan deciding whether a declarator declares a FUNCTION (vs a
// variable or function-pointer). A function declaration's name precedes its
// parameter parenthesis (`f(args)`, `*f(args)`); a function-pointer variable
// parenthesizes the name first (`(*fp)(args)`). So: the first '(' falls AFTER
// the first identifier, and a parameter list exists.
typedef struct {
    bool seen_id;
    int  decision; // 0 = undecided, 1 = function, 2 = not-a-function
} fn_scan_t;

static void
fn_scan(ncc_parse_tree_t *node, fn_scan_t *s)
{
    if (!node || s->decision != 0) {
        return;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        ncc_string_t      t   = (tok && ncc_option_is_set(tok->value))
                                    ? ncc_option_get(tok->value)
                                    : ncc_string_empty();
        if (tok && tok->tid == NCC_TOK_IDENTIFIER) {
            s->seen_id = true;
        }
        else if (t.data && strcmp(t.data, "(") == 0) {
            s->decision = s->seen_id ? 1 : 2;
        }
        return;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc && s->decision == 0; i++) {
        fn_scan(ncc_tree_child(node, i), s);
    }
}

static bool
declarator_is_function(ncc_parse_tree_t *declarator)
{
    if (!declarator) {
        return false;
    }
    // decision == 1 requires a '(' after an identifier — i.e. a parameter list
    // on the declared name, so no separate param-list check is needed.
    fn_scan_t s = {.seen_id = false, .decision = 0};
    fn_scan(declarator, &s);
    return s.decision == 1;
}

static void
add_named(ncc_symtab_t *st, ncc_string_t ns, ncc_token_info_t *name_tok,
          ncc_sym_kind_t kind, ncc_parse_tree_t *decl_node,
          ncc_parse_tree_t *type_node)
{
    if (!name_tok) {
        return;
    }
    ncc_string_t name = tok_text(name_tok);
    if (!name.data || name.u8_bytes == 0) {
        return;
    }
    ncc_sym_entry_t *e = ncc_symtab_add(st, ns, name, kind, decl_node);
    if (e) {
        e->type_node = type_node;
    }
}

// Register a struct/union/enum tag (with body) found in a specifier subtree.
static void
record_tag(ncc_symtab_t *st, ncc_parse_tree_t *spec, int64_t id_tid)
{
    ncc_parse_tree_t *tag = child_nt(spec, "tag_name");
    if (!tag) {
        return;
    }
    ncc_token_info_t *tok = declared_name_tok(tag, id_tid);
    add_named(st, ns_tag(), tok, NCC_SYM_TAG, spec, spec);
}

// Register enumerators of an enum specifier as integer constants.
static void
record_enum_constants(ncc_symtab_t *st, ncc_parse_tree_t *enum_spec,
                      int64_t id_tid)
{
    ncc_parse_tree_t *list = child_nt(enum_spec, "enumerator_list");
    if (!list) {
        return;
    }
    size_t nc = ncc_tree_num_children(list);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(list, i);
        if (nt_is(child, "enumerator")) {
            ncc_token_info_t *tok = declared_name_tok(child, id_tid);
            add_named(st, ns_ord(), tok, NCC_SYM_ENUM_CONST, child, enum_spec);
        }
        else {
            // enumerator_list is left-recursive; recurse to reach nested ones.
            record_enum_constants(st, child, id_tid);
        }
    }
}

// Walk specifiers for any struct/union/enum tag definitions + enum constants,
// recording them regardless of whether the enclosing declaration declares a
// name. (e.g. `struct foo { ... };` or a tag used inline in a variable's type.)
static void
record_specifier_tags(ncc_symtab_t *st, ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    if (nt_is(node, "struct_or_union_specifier")
        && child_nt(node, "member_declaration_list")) {
        record_tag(st, node, id_tid);
    }
    else if (nt_is(node, "enum_specifier")
             && child_nt(node, "enumerator_list")) {
        record_tag(st, node, id_tid);
        record_enum_constants(st, node, id_tid);
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        record_specifier_tags(st, ncc_tree_child(node, i), id_tid);
    }
}

// Record each init_declarator of a `declaration` as a typedef / function /
// variable, keyed by the base specifiers.
static void
record_declaration(ncc_symtab_t *st, ncc_parse_tree_t *decl, int64_t id_tid)
{
    ncc_parse_tree_t *specs = child_nt(decl, "declaration_specifiers");
    ncc_parse_tree_t *list  = child_nt(decl, "init_declarator_list");

    // Tags / enum constants in the specifiers are visible even with no
    // declarator (e.g. a bare `struct foo { ... };`).
    if (specs) {
        record_specifier_tags(st, specs, id_tid);
    }
    if (!specs || !list) {
        return;
    }

    bool is_typedef = subtree_has_token(specs, "typedef");

    size_t nc = ncc_tree_num_children(list);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(list, i);
        ncc_parse_tree_t *id    = nt_is(child, "init_declarator")
                                    ? child
                                    : (nt_is(child, "declarator") ? child
                                                                  : nullptr);
        if (!id) {
            // group / left-recursive list node — recurse one level.
            if (!ncc_tree_is_leaf(child)) {
                size_t gc = ncc_tree_num_children(child);
                for (size_t j = 0; j < gc; j++) {
                    ncc_parse_tree_t *gch = ncc_tree_child(child, j);
                    if (nt_is(gch, "init_declarator")
                        || nt_is(gch, "declarator")) {
                        id = gch;
                        break;
                    }
                }
            }
            if (!id) {
                continue;
            }
        }
        ncc_parse_tree_t *declarator = nt_is(id, "declarator")
                                         ? id
                                         : child_nt(id, "declarator");
        if (!declarator) {
            continue;
        }
        ncc_token_info_t *name = declared_name_tok(declarator, id_tid);

        ncc_sym_kind_t kind = is_typedef          ? NCC_SYM_TYPEDEF
                            : declarator_is_function(declarator)
                                ? NCC_SYM_FUNCTION
                                : NCC_SYM_VARIABLE;
        add_named(st, ns_ord(), name, kind, declarator, specs);
    }
}

// Record every parameter_declaration anywhere under a (left-recursive)
// parameter list into the current scope.
static void
scan_params(ncc_symtab_t *st, ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }
    if (nt_is(node, "parameter_declaration")) {
        ncc_parse_tree_t *specs = child_nt(node, "declaration_specifiers");
        ncc_parse_tree_t *declr = child_nt(node, "declarator");
        ncc_token_info_t *name  = declr ? declared_name_tok(declr, id_tid)
                                        : nullptr;
        add_named(st, ns_ord(), name, NCC_SYM_PARAM, declr ? declr : node, specs);
        return; // do not descend (nested declarators are this param's own)
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_params(st, ncc_tree_child(node, i), id_tid);
    }
}

// Record the parameters of a function declarator into the current scope.
static void
record_parameters(ncc_symtab_t *st, ncc_parse_tree_t *declarator, int64_t id_tid)
{
    ncc_parse_tree_t *ptl = child_nt(declarator, "parameter_type_list");
    if (ptl) {
        scan_params(st, ptl, id_tid);
    }
}

// ============================================================================
// Scope-aware DFS
// ============================================================================

static void
walk(ncc_symtab_t *st, ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    bool pushed = false;
    if (nt_is(node, "function_definition") || nt_is(node, "compound_statement")) {
        ncc_symtab_push_scope(st, ns_ord(), ncc_string_empty());
        ncc_symtab_push_scope(st, ns_tag(), ncc_string_empty());
        pushed = true;
    }

    if (nt_is(node, "declaration")) {
        record_declaration(st, node, id_tid);
    }
    else if (nt_is(node, "function_definition")) {
        // Return type + name, then parameters into the just-pushed scope.
        ncc_parse_tree_t *specs = child_nt(node, "declaration_specifiers");
        ncc_parse_tree_t *declr = child_nt(node, "declarator");
        if (specs) {
            record_specifier_tags(st, specs, id_tid);
        }
        if (declr) {
            // The function name binds in the ENCLOSING scope, but recording it
            // in the body scope still makes it resolvable while typing the
            // body; the enclosing file-scope prototype (if any) also recorded
            // it. Good enough for the type model's needs.
            ncc_token_info_t *name = declared_name_tok(declr, id_tid);
            add_named(st, ns_ord(), name, NCC_SYM_FUNCTION, declr, specs);
            record_parameters(st, declr, id_tid);
        }
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        walk(st, ncc_tree_child(node, i), id_tid);
    }

    if (pushed) {
        ncc_symtab_pop_scope(st, ns_ord());
        ncc_symtab_pop_scope(st, ns_tag());
    }
}

ncc_symtab_t *
ncc_populate_symbols(ncc_grammar_t *g, ncc_parse_tree_t *tree)
{
    ncc_symtab_t *st = ncc_symtab_new();
    if (!st || !tree) {
        return st;
    }
    int64_t id_tid = identifier_tid(g);

    // Global scope so file-scope names live in a scope chain (consistent
    // shadowing with nested scopes).
    ncc_symtab_push_scope(st, ns_ord(), ncc_string_empty());
    ncc_symtab_push_scope(st, ns_tag(), ncc_string_empty());

    walk(st, tree, id_tid);

    return st;
}
