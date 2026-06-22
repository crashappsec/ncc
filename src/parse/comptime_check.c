#include "parse/comptime_check.h"

#include "lib/alloc.h"
#include "lib/string.h"
#include "xform/transform.h"
#include "xform/xform_helpers.h"
#include "xform/xform_type_layout.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ncc_symtab_t *symtab;
    int           errors;
    bool          has_optional;
} comptime_check_ctx_t;

static bool
is_c_identifier(const char *s)
{
    if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return false;
    }

    for (const char *p = s + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') {
            return false;
        }
    }
    return true;
}

static ncc_string_t
borrowed_cstr_string(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    return (ncc_string_t){
        .data       = (char *)s,
        .u8_bytes   = len,
        .codepoints = len,
        .styling    = nullptr,
    };
}

static bool
leaf_is(ncc_parse_tree_t *node, const char *text)
{
    return node && ncc_tree_is_leaf(node)
        && ncc_xform_leaf_text_eq(node, text);
}

static bool
node_text_is(ncc_parse_tree_t *node, const char *text)
{
    if (leaf_is(node, text)) {
        return true;
    }

    ncc_string_t t = ncc_xform_node_to_text(node);
    bool         r = t.data && strcmp(t.data, text) == 0;
    ncc_free(t.data);
    return r;
}

static const char *
identifier_text(ncc_parse_tree_t *node)
{
    if (!node || !ncc_xform_nt_name_is(node, "identifier")) {
        return nullptr;
    }

    const char *text = ncc_xform_get_first_leaf_text(node);
    return is_c_identifier(text) ? text : nullptr;
}

static ncc_parse_tree_t *
single_child(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node) || ncc_tree_num_children(node) != 1) {
        return nullptr;
    }
    return ncc_tree_child(node, 0);
}

static const char *
mutation_root_ident(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    const char *ident = identifier_text(node);
    if (ident) {
        return ident;
    }

    if (ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);

    if (ncc_xform_nt_name_is(node, "primary_expression")) {
        if (nc >= 3 && leaf_is(ncc_tree_child(node, 0), "(")
            && leaf_is(ncc_tree_child(node, nc - 1), ")")) {
            return mutation_root_ident(ncc_tree_child(node, 1));
        }
    }

    if (ncc_xform_nt_name_is(node, "postfix_expression")) {
        if (nc >= 3) {
            ncc_parse_tree_t *op = ncc_tree_child(node, 1);
            if (leaf_is(op, ".") || leaf_is(op, "->") || leaf_is(op, "[")) {
                return mutation_root_ident(ncc_tree_child(node, 0));
            }
            if (leaf_is(op, "(")) {
                return nullptr;
            }
        }

        if (nc >= 2) {
            ncc_parse_tree_t *last = ncc_tree_child(node, nc - 1);
            if (leaf_is(last, "++") || leaf_is(last, "--")) {
                return mutation_root_ident(ncc_tree_child(node, 0));
            }
        }
    }

    if (ncc_xform_nt_name_is(node, "unary_expression")) {
        if (nc >= 2) {
            ncc_parse_tree_t *op = ncc_tree_child(node, 0);
            if (node_text_is(op, "++") || node_text_is(op, "--")
                || node_text_is(op, "*") || node_text_is(op, "&")) {
                return mutation_root_ident(ncc_tree_child(node, 1));
            }
        }
    }

    ncc_parse_tree_t *only = single_child(node);
    if (only) {
        return mutation_root_ident(only);
    }

    return nullptr;
}

static ncc_scope_t *
scope_for_node(ncc_symtab_t *symtab, ncc_parse_tree_t *node)
{
    for (ncc_parse_tree_t *cur = node; cur && !ncc_tree_is_leaf(cur);) {
        ncc_scope_t *scope = ncc_symtab_scope_for_node(symtab, cur);
        if (scope) {
            return scope;
        }
        cur = (ncc_parse_tree_t *)ncc_tree_node_value(cur).parent;
    }
    return nullptr;
}

static ncc_sym_entry_t *
lookup_mutation_root(ncc_symtab_t *symtab, ncc_parse_tree_t *site,
                     const char *name)
{
    if (!symtab || !name) {
        return nullptr;
    }

    ncc_scope_t  *scope = scope_for_node(symtab, site);
    ncc_string_t ns     = borrowed_cstr_string("");
    ncc_string_t key    = borrowed_cstr_string(name);
    return ncc_symtab_lookup_scoped(symtab, scope, ns, key);
}

static bool
inside_comptime_main(ncc_parse_tree_t *site)
{
    ncc_parse_tree_t *fn = ncc_xform_find_ancestor(site, "function_definition");
    if (!fn) {
        return false;
    }

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(fn, "declarator");
    char             *name       = ncc_layout_declarator_name(declarator);
    bool              r          = name && strcmp(name, "comptime_main") == 0;
    ncc_free(name);
    return r;
}

static char *
function_definition_name(ncc_parse_tree_t *fn)
{
    if (!fn || !ncc_xform_nt_name_is(fn, "function_definition")) {
        return nullptr;
    }

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(fn, "declarator");
    return ncc_layout_declarator_name(declarator);
}

static void
report_illegal_mutation(comptime_check_ctx_t *ctx, ncc_parse_tree_t *site,
                        const char *name)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(site, &line, &col);

    fprintf(stderr,
            "ncc: error:%s%u:%u: comptime variable '%s' may only be "
            "assigned in comptime_main\n",
            line ? " " : "", line, col, name);
    ctx->errors++;
}

static void
report_illegal_optional(comptime_check_ctx_t *ctx, ncc_parse_tree_t *site)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(site, &line, &col);

    fprintf(stderr,
            "ncc: error:%s%u:%u: [[n00b::optional]] is only valid on a "
            "comptime_main definition\n",
            line ? " " : "", line, col);
    ctx->errors++;
}

static void
check_mutation(comptime_check_ctx_t *ctx, ncc_parse_tree_t *site,
               ncc_parse_tree_t *target)
{
    if (!ctx || !site || !target || inside_comptime_main(site)) {
        return;
    }

    const char *name = mutation_root_ident(target);
    if (!name) {
        return;
    }

    ncc_sym_entry_t *entry = lookup_mutation_root(ctx->symtab, site, name);
    if (!entry || entry->kind != NCC_SYM_VARIABLE || !entry->is_comptime) {
        return;
    }

    report_illegal_mutation(ctx, target, name);
}

static bool
optional_attr_carrying_node(ncc_parse_tree_t *node)
{
    return node && !ncc_tree_is_leaf(node)
        && ncc_xform_nt_name_is(node, "attribute_specifier_sequence")
        && ncc_xform_subtree_carries_n00b_named_attr(node, "optional");
}

static bool
optional_parent_boundary(ncc_parse_tree_t *node)
{
    return ncc_xform_nt_name_is(node, "declaration")
        || ncc_xform_nt_name_is(node, "parameter_declaration")
        || ncc_xform_nt_name_is(node, "struct_declaration")
        || ncc_xform_nt_name_is(node, "enumerator")
        || ncc_xform_nt_name_is(node, "compound_statement")
        || ncc_xform_nt_name_is(node, "statement");
}

static ncc_parse_tree_t *
optional_owner_function_definition(ncc_parse_tree_t *attr)
{
    for (ncc_parse_tree_t *cur = attr; cur && !ncc_tree_is_leaf(cur);) {
        ncc_parse_tree_t *parent =
            (ncc_parse_tree_t *)ncc_tree_node_value(cur).parent;
        if (!parent || ncc_tree_is_leaf(parent)) {
            return nullptr;
        }
        if (ncc_xform_nt_name_is(parent, "function_definition")) {
            return parent;
        }
        if (optional_parent_boundary(parent)) {
            return nullptr;
        }
        cur = parent;
    }
    return nullptr;
}

static void
check_optional_attr(comptime_check_ctx_t *ctx, ncc_parse_tree_t *attr)
{
    ncc_parse_tree_t *fn = optional_owner_function_definition(attr);
    char             *name = function_definition_name(fn);
    bool              valid = name && strcmp(name, "comptime_main") == 0;

    if (valid) {
        ctx->has_optional = true;
    }
    else {
        report_illegal_optional(ctx, attr);
    }

    ncc_free(name);
}

static bool
assignment_operator_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "assignment_operator")) {
        return false;
    }

    ncc_string_t t = ncc_xform_node_to_text(node);
    bool         r = t.data && strchr(t.data, '=') != nullptr;
    ncc_free(t.data);
    return r;
}

static void
walk(comptime_check_ctx_t *ctx, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);

    if (optional_attr_carrying_node(node)) {
        check_optional_attr(ctx, node);
    }

    if (ncc_xform_nt_name_is(node, "assignment_expression") && nc >= 3) {
        for (size_t i = 1; i + 1 < nc; i++) {
            if (assignment_operator_node(ncc_tree_child(node, i))) {
                check_mutation(ctx, node, ncc_tree_child(node, i - 1));
                break;
            }
        }
    }
    else if (ncc_xform_nt_name_is(node, "unary_expression") && nc >= 2) {
        ncc_parse_tree_t *op = ncc_tree_child(node, 0);
        if (node_text_is(op, "++") || node_text_is(op, "--")) {
            check_mutation(ctx, node, ncc_tree_child(node, 1));
        }
    }
    else if (ncc_xform_nt_name_is(node, "postfix_expression") && nc >= 2) {
        ncc_parse_tree_t *op = ncc_tree_child(node, nc - 1);
        if (node_text_is(op, "++") || node_text_is(op, "--")) {
            check_mutation(ctx, node, ncc_tree_child(node, 0));
        }
    }

    for (size_t i = 0; i < nc; i++) {
        walk(ctx, ncc_tree_child(node, i));
    }
}

int
ncc_comptime_check(ncc_grammar_t *g, ncc_parse_tree_t *tree,
                   ncc_symtab_t *symtab, bool *has_optional_out)
{
    (void)g;

    if (has_optional_out) {
        *has_optional_out = false;
    }

    comptime_check_ctx_t ctx = {
        .symtab = symtab,
        .errors = 0,
        .has_optional = false,
    };
    walk(&ctx, tree);
    if (ctx.errors == 0 && ctx.has_optional) {
        ncc_xform_strip_n00b_named_attribute_specifiers(tree, "optional");
    }
    if (has_optional_out) {
        *has_optional_out = ctx.errors == 0 && ctx.has_optional;
    }
    return ctx.errors;
}
