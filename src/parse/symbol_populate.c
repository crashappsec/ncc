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

static bool
subtree_has_token_prefix(ncc_parse_tree_t *node, const char *prefix)
{
    if (!node || !prefix) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);
            size_t prefix_len = strlen(prefix);
            return val.data && val.u8_bytes >= prefix_len
                && strncmp(val.data, prefix, prefix_len) == 0;
        }
        return false;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_token_prefix(ncc_tree_child(node, i), prefix)) {
            return true;
        }
    }
    return false;
}

static bool
subtree_has_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || !name || ncc_tree_is_leaf(node)) {
        return false;
    }
    if (nt_is(node, name)) {
        return true;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_nt(ncc_tree_child(node, i), name)) {
            return true;
        }
    }
    return false;
}

static const char *
first_leaf_text(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (!tok || !ncc_option_is_set(tok->value)) {
            return nullptr;
        }
        ncc_string_t val = ncc_option_get(tok->value);
        return val.data;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        const char *text = first_leaf_text(ncc_tree_child(node, i));
        if (text) {
            return text;
        }
    }
    return nullptr;
}

static bool
declarator_has_pointer(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t val = ncc_option_get(tok->value);
            return val.data && strcmp(val.data, "*") == 0;
        }
        return false;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (declarator_has_pointer(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

static bool
attribute_leaf_scan(ncc_parse_tree_t *node, const char *name, bool *saw_n00b)
{
    if (!node || !name) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (!tok || !ncc_option_is_set(tok->value)) {
            return false;
        }
        ncc_string_t text = ncc_option_get(tok->value);
        if (!text.data) {
            return false;
        }
        if (!*saw_n00b && strcmp(text.data, "n00b") == 0) {
            *saw_n00b = true;
            return false;
        }
        return *saw_n00b && strcmp(text.data, name) == 0;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (attribute_leaf_scan(ncc_tree_child(node, i), name, saw_n00b)) {
            return true;
        }
    }

    return false;
}

static bool
attribute_is_n00b_named(ncc_parse_tree_t *node, const char *name)
{
    bool saw_n00b = false;
    return attribute_leaf_scan(node, name, &saw_n00b);
}

static bool
subtree_carries_n00b_named_attr(ncc_parse_tree_t *node, const char *name)
{
    if (!node || !name || ncc_tree_is_leaf(node)) {
        return false;
    }

    if (nt_is(node, "attribute")
        && attribute_is_n00b_named(node, name)) {
        return true;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_carries_n00b_named_attr(ncc_tree_child(node, i), name)) {
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

static ncc_sym_entry_t *
add_named(ncc_symtab_t *st, ncc_string_t ns, ncc_token_info_t *name_tok,
          ncc_sym_kind_t kind, ncc_parse_tree_t *decl_node,
          ncc_parse_tree_t *type_node)
{
    if (!name_tok) {
        return nullptr;
    }
    ncc_string_t name = tok_text(name_tok);
    if (!name.data || name.u8_bytes == 0) {
        return nullptr;
    }
    ncc_sym_entry_t *e = ncc_symtab_add(st, ns, name, kind, decl_node);
    if (e) {
        e->type_node = type_node;
    }
    return e;
}

// The name token of a tag_name node, accepting either an IDENTIFIER or a
// TYPEDEF_NAME leaf. A tag whose name coincides with a typedef of the same name
// — C's ubiquitous `typedef struct X X;` — tokenizes the tag name as
// TYPEDEF_NAME, which declared_name_tok (IDENTIFIER-only) would miss, leaving
// the tag unrecorded and its members unresolvable.
static ncc_token_info_t *
tag_name_tok(ncc_parse_tree_t *node, int64_t id_tid)
{
    if (!node) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok
            && (tok->tid == (int32_t)id_tid
                || tok->tid == NCC_TOK_TYPEDEF_NAME)) {
            return tok;
        }
        return nullptr;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *t = tag_name_tok(ncc_tree_child(node, i), id_tid);
        if (t) {
            return t;
        }
    }
    return nullptr;
}

// Register a struct/union/enum tag (with body) found in a specifier subtree.
static void
record_tag(ncc_symtab_t *st, ncc_parse_tree_t *spec, int64_t id_tid)
{
    ncc_parse_tree_t *tag = child_nt(spec, "tag_name");
    if (!tag) {
        return;
    }
    // A _generic_struct's tag is a `typeid(...)` expression, not a simple name
    // (e.g. n00b_list_t(T) is `typeid("n00b_list", T)`). Recording it here would
    // grab the type argument T as the tag name and shadow T's real struct with
    // the list's body. Its concrete minted tag is registered during
    // generic_struct lowering, so skip it in this pass.
    if (subtree_has_token(tag, "typeid")) {
        return;
    }
    ncc_token_info_t *tok = tag_name_tok(tag, id_tid);
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

static bool
leaf_text_is(ncc_parse_tree_t *node, const char *text)
{
    if (!node || !text || !ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(node);
    if (!tok || !ncc_option_is_set(tok->value)) {
        return false;
    }

    ncc_string_t val = ncc_option_get(tok->value);
    return val.data && strcmp(val.data, text) == 0;
}

static bool
node_directly_has_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (leaf_text_is(ncc_tree_child(node, i), text)) {
            return true;
        }
    }
    return false;
}

static bool
postfix_expression_is_call(ncc_parse_tree_t *node)
{
    if (!nt_is(node, "postfix_expression")
        || !node_directly_has_leaf_text(node, "(")) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (nt_is(ncc_tree_child(node, i), "postfix_expression")) {
            return true;
        }
    }
    return false;
}

typedef struct {
    bool needs_bake;
    bool needs_host_exec;
} static_init_class_t;

static static_init_class_t
initializer_static_bake_class(ncc_parse_tree_t *init)
{
    static_init_class_t result = {0};

    if (!init || ncc_tree_is_leaf(init)) {
        return result;
    }
    if (subtree_has_token_prefix(init, "__ncc_")) {
        return result;
    }
    if (nt_is(init, "unary_expression")) {
        ncc_parse_tree_t *first = ncc_tree_num_children(init) > 0
                                ? ncc_tree_child(init, 0)
                                : nullptr;
        if (leaf_text_is(first, "sizeof")
            || leaf_text_is(first, "_Countof")
            || leaf_text_is(first, "alignof")
            || leaf_text_is(first, "_Alignof")) {
            return result;
        }
    }

    if (postfix_expression_is_call(init)) {
        result.needs_bake = true;
        result.needs_host_exec = true;
        return result;
    }

    size_t nc = ncc_tree_num_children(init);
    for (size_t i = 0; i < nc; i++) {
        static_init_class_t child =
            initializer_static_bake_class(ncc_tree_child(init, i));
        result.needs_bake = result.needs_bake || child.needs_bake;
        result.needs_host_exec = result.needs_host_exec
                              || child.needs_host_exec;
    }
    return result;
}

static bool
initializer_is_buffer_static_image(ncc_parse_tree_t *specs,
                                   ncc_parse_tree_t *declarator,
                                   ncc_parse_tree_t *init)
{
    if (!specs || !declarator || !init) {
        return false;
    }
    if (!subtree_has_token(specs, "n00b_buffer_t")
        || !declarator_has_pointer(declarator)) {
        return false;
    }
    return subtree_has_token(init, "__ncc_buflit");
}

static bool
initializer_has_array_literal(ncc_parse_tree_t *init)
{
    if (!init) {
        return false;
    }
    if (subtree_has_nt(init, "array_literal")) {
        return true;
    }

    ncc_parse_tree_t *modified = child_nt(init, "modified_literal");
    if (!modified) {
        return false;
    }
    const char *modifier = first_leaf_text(modified);
    return modifier && strcmp(modifier, "a") == 0;
}

static bool
initializer_has_list_literal(ncc_parse_tree_t *init)
{
    if (!init) {
        return false;
    }
    if (subtree_has_nt(init, "list_literal")) {
        return true;
    }

    ncc_parse_tree_t *modified = child_nt(init, "modified_literal");
    if (!modified) {
        return false;
    }
    const char *modifier = first_leaf_text(modified);
    return modifier && strcmp(modifier, "l") == 0;
}

static bool
initializer_has_dict_literal(ncc_parse_tree_t *init)
{
    if (!init) {
        return false;
    }
    if (subtree_has_nt(init, "dict_literal")) {
        return true;
    }

    ncc_parse_tree_t *modified = child_nt(init, "modified_literal");
    if (!modified) {
        return false;
    }
    const char *modifier = first_leaf_text(modified);
    return modifier && strcmp(modifier, "d") == 0;
}

static bool
specs_are_migrated_n00b_array_value(ncc_parse_tree_t *specs,
                                    ncc_parse_tree_t *declarator)
{
    if (!specs || !declarator || declarator_has_pointer(declarator)) {
        return false;
    }
    if (subtree_has_token_prefix(specs, "ncc_array_")) {
        return false;
    }
    return subtree_has_token(specs, "typeid")
        && subtree_has_token(specs, "\"array\"")
        && subtree_has_token(specs, "data")
        && subtree_has_token(specs, "len")
        && subtree_has_token(specs, "cap")
        && subtree_has_token(specs, "scan_kind");
}

static ncc_sym_entry_t *
first_typedef_ref_in_specs(ncc_symtab_t *st, ncc_parse_tree_t *specs)
{
    if (!st || !specs) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(specs)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(specs);
        if (!tok || !ncc_option_is_set(tok->value)) {
            return nullptr;
        }
        ncc_string_t name = ncc_option_get(tok->value);
        ncc_sym_entry_t *entry =
            ncc_symtab_lookup(st, ncc_string_empty(), name);
        return entry && entry->kind == NCC_SYM_TYPEDEF ? entry : nullptr;
    }

    size_t nc = ncc_tree_num_children(specs);
    for (size_t i = 0; i < nc; i++) {
        ncc_sym_entry_t *entry =
            first_typedef_ref_in_specs(st, ncc_tree_child(specs, i));
        if (entry) {
            return entry;
        }
    }
    return nullptr;
}

static bool
typedef_specs_can_alias_container(ncc_parse_tree_t *specs)
{
    return specs != nullptr
        && !subtree_has_nt(specs, "member_declaration_list")
        && !subtree_has_nt(specs, "enumerator_list");
}

static bool
specs_are_migrated_n00b_array_value_resolved(ncc_symtab_t *st,
                                             ncc_parse_tree_t *specs,
                                             ncc_parse_tree_t *declarator,
                                             int depth)
{
    if (specs_are_migrated_n00b_array_value(specs, declarator)) {
        return true;
    }
    if (!st || !specs || !declarator || depth <= 0
        || declarator_has_pointer(declarator)
        || !typedef_specs_can_alias_container(specs)) {
        return false;
    }

    ncc_sym_entry_t *entry = first_typedef_ref_in_specs(st, specs);
    return entry && entry->type_node && entry->decl_node
        && specs_are_migrated_n00b_array_value_resolved(st,
                                                        entry->type_node,
                                                        entry->decl_node,
                                                        depth - 1);
}

static bool
specs_are_migrated_n00b_list_value(ncc_parse_tree_t *specs,
                                   ncc_parse_tree_t *declarator)
{
    if (!specs || !declarator || declarator_has_pointer(declarator)) {
        return false;
    }
    return subtree_has_token(specs, "typeid")
        && subtree_has_token(specs, "\"n00b_list\"")
        && subtree_has_token(specs, "data")
        && subtree_has_token(specs, "len")
        && subtree_has_token(specs, "cap")
        && subtree_has_token(specs, "lock")
        && subtree_has_token(specs, "scan_kind");
}

static bool
specs_are_migrated_n00b_list_value_resolved(ncc_symtab_t *st,
                                            ncc_parse_tree_t *specs,
                                            ncc_parse_tree_t *declarator,
                                            int depth)
{
    if (specs_are_migrated_n00b_list_value(specs, declarator)) {
        return true;
    }
    if (!st || !specs || !declarator || depth <= 0
        || declarator_has_pointer(declarator)
        || !typedef_specs_can_alias_container(specs)) {
        return false;
    }

    ncc_sym_entry_t *entry = first_typedef_ref_in_specs(st, specs);
    return entry && entry->type_node && entry->decl_node
        && specs_are_migrated_n00b_list_value_resolved(st,
                                                       entry->type_node,
                                                       entry->decl_node,
                                                       depth - 1);
}

static bool
specs_are_migrated_n00b_dict_value(ncc_parse_tree_t *specs,
                                   ncc_parse_tree_t *declarator)
{
    if (!specs || !declarator || declarator_has_pointer(declarator)) {
        return false;
    }
    return subtree_has_token(specs, "typeid")
        && subtree_has_token(specs, "\"n00b_dict\"");
}

static bool
specs_are_migrated_n00b_dict_value_resolved(ncc_symtab_t *st,
                                            ncc_parse_tree_t *specs,
                                            ncc_parse_tree_t *declarator,
                                            int depth)
{
    if (specs_are_migrated_n00b_dict_value(specs, declarator)) {
        return true;
    }
    if (!st || !specs || !declarator || depth <= 0
        || declarator_has_pointer(declarator)
        || !typedef_specs_can_alias_container(specs)) {
        return false;
    }

    ncc_sym_entry_t *entry = first_typedef_ref_in_specs(st, specs);
    return entry && entry->type_node && entry->decl_node
        && specs_are_migrated_n00b_dict_value_resolved(st,
                                                       entry->type_node,
                                                       entry->decl_node,
                                                       depth - 1);
}

static bool
initializer_is_n00b_array_literal_static_init(ncc_symtab_t *st,
                                              ncc_parse_tree_t *specs,
                                              ncc_parse_tree_t *declarator,
                                              ncc_parse_tree_t *init)
{
    return specs_are_migrated_n00b_array_value_resolved(st, specs,
                                                        declarator, 8)
        && initializer_has_array_literal(init);
}

static bool
initializer_is_n00b_list_literal_static_init(ncc_symtab_t *st,
                                             ncc_parse_tree_t *specs,
                                             ncc_parse_tree_t *declarator,
                                             ncc_parse_tree_t *init)
{
    return specs_are_migrated_n00b_list_value_resolved(st, specs,
                                                       declarator, 8)
        && initializer_has_list_literal(init);
}

static bool
initializer_is_n00b_dict_literal_static_init(ncc_symtab_t *st,
                                             ncc_parse_tree_t *specs,
                                             ncc_parse_tree_t *declarator,
                                             ncc_parse_tree_t *init)
{
    return specs_are_migrated_n00b_dict_value_resolved(st, specs,
                                                       declarator, 8)
        && initializer_has_dict_literal(init);
}

static bool
name_is_ncc_generated(ncc_string_t name)
{
    return name.data && name.u8_bytes >= 6
        && strncmp(name.data, "__ncc_", 6) == 0;
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

    bool is_typedef  = subtree_has_token(specs, "typedef");
    bool is_file_scope = ncc_symtab_depth(st, ns_ord()) == 1;
    bool decl_comptime = subtree_carries_n00b_named_attr(decl, "comptime");

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
        ncc_sym_entry_t *entry = add_named(st, ns_ord(), name, kind,
                                           declarator, specs);
        if (entry && kind == NCC_SYM_VARIABLE && is_file_scope
            && (decl_comptime
                || subtree_carries_n00b_named_attr(id, "comptime"))) {
            entry->is_comptime = true;
        }
        if (entry && kind == NCC_SYM_VARIABLE && is_file_scope
            && !entry->is_comptime && nt_is(id, "init_declarator")) {
            ncc_parse_tree_t *init = child_nt(id, "initializer");
            static_init_class_t init_class = {0};
            if (initializer_is_buffer_static_image(specs, declarator, init)) {
                init_class.needs_bake = true;
                init_class.needs_host_exec = true;
            }
            else if (initializer_is_n00b_array_literal_static_init(st,
                                                                   specs,
                                                                   declarator,
                                                                   init)) {
                init_class.needs_bake = true;
                init_class.needs_host_exec = true;
            }
            else if (initializer_is_n00b_list_literal_static_init(st,
                                                                  specs,
                                                                  declarator,
                                                                  init)) {
                init_class.needs_bake = true;
                init_class.needs_host_exec = true;
            }
            else if (initializer_is_n00b_dict_literal_static_init(st,
                                                                  specs,
                                                                  declarator,
                                                                  init)) {
                init_class.needs_bake = true;
                init_class.needs_host_exec = true;
            }
            else {
                init_class = initializer_static_bake_class(init);
            }
            if (!name_is_ncc_generated(entry->name) && init_class.needs_bake) {
                entry->is_static_init = true;
                entry->static_init_needs_host_exec =
                    init_class.needs_host_exec;
            }
        }
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

// Record the parameters of a function declarator into the current scope. The
// parameter_type_list nests under direct_declarator/function_declarator (not a
// direct child reachable by child_nt, which only traverses group wrappers), so
// scan the whole declarator for parameter_declarations directly. scan_params
// does not descend into a parameter's own nested declarators, so only this
// function's parameters are recorded.
static void
record_parameters(ncc_symtab_t *st, ncc_parse_tree_t *declarator, int64_t id_tid)
{
    scan_params(st, declarator, id_tid);
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

    // Bind a function definition's name in the ENCLOSING scope, before the body
    // scope is pushed, so it stays resolvable after the body is popped — needed
    // for call typing and for type-aware parse disambiguation of bare-call
    // statements (`f(x);`) when the definition has no separate prototype.
    if (nt_is(node, "function_definition")) {
        ncc_parse_tree_t *fdeclr = child_nt(node, "declarator");
        if (fdeclr) {
            ncc_parse_tree_t *fspecs = child_nt(node, "declaration_specifiers");
            ncc_token_info_t *fname  = declared_name_tok(fdeclr, id_tid);
            add_named(st, ns_ord(), fname, NCC_SYM_FUNCTION, fdeclr, fspecs);
        }
    }

    bool pushed = false;
    if (nt_is(node, "function_definition") || nt_is(node, "compound_statement")) {
        ncc_symtab_push_scope(st, ns_ord(), ncc_string_empty());
        ncc_symtab_push_scope(st, ns_tag(), ncc_string_empty());
        // Tag the value (ord) scope with its creating node so expression typing
        // can map an expression back to this lexical scope after the walk.
        ncc_symtab_set_scope_node(st, ns_ord(), node);
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
            // Also bind the name in the body scope (it is already bound in the
            // enclosing scope above) so it resolves while typing the body, e.g.
            // recursive calls.
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
