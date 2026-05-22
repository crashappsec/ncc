// xform_gc_stack_maps.c -- Discover n00b GC stack roots for later emission.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
copy_cstr(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    char  *r   = ncc_alloc_size(1, len + 1);
    if (len > 0) {
        memcpy(r, s, len);
    }
    r[len] = '\0';
    return r;
}

static char *
trim_copy(const char *s)
{
    if (!s) {
        return copy_cstr("");
    }

    while (isspace((unsigned char)*s)) {
        s++;
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        len--;
    }

    char *result = ncc_alloc_size(1, len + 1);
    memcpy(result, s, len);
    result[len] = '\0';
    return result;
}

static char *
node_text(ncc_parse_tree_t *node)
{
    ncc_string_t s = ncc_xform_node_to_text(node);
    char        *r = trim_copy(s.data);
    ncc_free(s.data);
    return r;
}

static bool
contains_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, text);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (contains_leaf_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

static int
count_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, text) ? 1 : 0;
    }

    int    result = 0;
    size_t nc     = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        result += count_leaf_text(ncc_tree_child(node, i), text);
    }

    return result;
}

static ncc_parse_tree_t *
first_descendant_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found = first_descendant_nt(ncc_tree_child(node, i),
                                                      name);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static char *
declarator_name(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (text && (isalpha((unsigned char)text[0]) || text[0] == '_')) {
            return copy_cstr(text);
        }
        return nullptr;
    }

    char  *last = nullptr;
    size_t nc   = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = declarator_name(ncc_tree_child(node, i));
        if (candidate) {
            ncc_free(last);
            last = candidate;
        }
    }
    return last;
}

static bool
is_ident_start_char(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool
is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static char *
copy_last_identifier_before(const char *text, size_t end)
{
    size_t i = end;

    while (i > 0) {
        while (i > 0 && !is_ident_char(text[i - 1])) {
            i--;
        }
        size_t ident_end = i;
        while (i > 0 && is_ident_char(text[i - 1])) {
            i--;
        }
        if (ident_end > i && is_ident_start_char(text[i])) {
            size_t len  = ident_end - i;
            char  *name = ncc_alloc_size(1, len + 1);
            memcpy(name, text + i, len);
            name[len] = '\0';
            return name;
        }
    }

    return nullptr;
}

static size_t
find_last_top_level_param_open(const char *text)
{
    int    depth = 0;
    size_t last  = (size_t)-1;

    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '(') {
            if (depth == 0) {
                last = i;
            }
            depth++;
        }
        else if (text[i] == ')' && depth > 0) {
            depth--;
        }
    }

    return last;
}

static char *
function_name(ncc_parse_tree_t *declarator)
{
    char *text = node_text(declarator);
    if (!text) {
        return copy_cstr("<unknown>");
    }

    size_t param_open = find_last_top_level_param_open(text);
    size_t name_end   = param_open == (size_t)-1 ? strlen(text) : param_open;
    char  *name       = copy_last_identifier_before(text, name_end);

    ncc_free(text);
    return name ? name : copy_cstr("<unknown>");
}

static void
gc_stack_errorf(ncc_parse_tree_t *node, const char *fmt, ...)
{
    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(node, &line, &col);

    fprintf(stderr, "ncc: error: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (line %u, col %u)\n", line, col);
    exit(1);
}

static bool
decl_is_stack_storage(ncc_parse_tree_t *decl_specs)
{
    return !contains_leaf_text(decl_specs, "static")
        && !contains_leaf_text(decl_specs, "extern")
        && !contains_leaf_text(decl_specs, "typedef");
}

static bool
decl_specs_are_aggregate(ncc_parse_tree_t *decl_specs)
{
    return first_descendant_nt(decl_specs, "struct_or_union_specifier")
        != nullptr;
}

static bool
parse_positive_integer_literal(const char *text, uint64_t *out)
{
    char *trimmed = trim_copy(text);
    if (!trimmed[0]) {
        ncc_free(trimmed);
        return false;
    }

    for (char *p = trimmed; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            ncc_free(trimmed);
            return false;
        }
    }

    char    *end = nullptr;
    uint64_t val = strtoull(trimmed, &end, 10);
    bool     ok  = end && *end == '\0' && val > 0;
    ncc_free(trimmed);

    if (ok) {
        *out = val;
    }
    return ok;
}

static bool
array_bound_product(ncc_parse_tree_t *node, uint64_t *product,
                    ncc_parse_tree_t **bad_array)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return true;
    }

    if (ncc_xform_nt_name_is(node, "array_declarator")) {
        ncc_parse_tree_t *bound = ncc_xform_find_child_nt(
            node, "assignment_expression");
        uint64_t count = 0;

        if (!bound) {
            *bad_array = node;
            return false;
        }

        char *text = node_text(bound);
        bool  ok   = parse_positive_integer_literal(text, &count);
        ncc_free(text);

        if (!ok) {
            *bad_array = node;
            return false;
        }

        *product *= count;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (!array_bound_product(ncc_tree_child(node, i), product,
                                 bad_array)) {
            return false;
        }
    }

    return true;
}

static bool
has_parenthesized_pointer_array(ncc_parse_tree_t *declarator)
{
    char *text = node_text(declarator);
    bool  result = strstr(text, "(*") != nullptr || strstr(text, "( *")
                                                     != nullptr;
    ncc_free(text);
    return result;
}

static ncc_gc_stack_root_t *
record_root(ncc_xform_ctx_t *ctx, const char *function, const char *name,
            ncc_parse_tree_t *type_node, ncc_gc_stack_root_kind_t kind,
            ncc_gc_stack_root_shape_t shape, uint64_t num_words,
            ncc_parse_tree_t *scope, ncc_parse_tree_t *decl_node)
{
    ncc_xform_data_t    *data = ncc_xform_get_data(ctx);
    ncc_gc_stack_root_t *root = ncc_alloc(ncc_gc_stack_root_t);

    uint32_t line = 0;
    uint32_t col  = 0;
    ncc_xform_first_leaf_pos(decl_node, &line, &col);

    *root = (ncc_gc_stack_root_t){
        .function_name = copy_cstr(function),
        .name          = copy_cstr(name),
        .type_text     = node_text(type_node),
        .kind          = kind,
        .shape         = shape,
        .num_words     = num_words,
        .line          = line,
        .col           = col,
        .scope         = scope,
        .declaration   = decl_node,
        .next          = data->gc_stack_roots,
    };

    data->gc_stack_roots = root;
    data->gc_stack_root_count++;
    return root;
}

static void
classify_declarator(ncc_xform_ctx_t *ctx, const char *function,
                    ncc_parse_tree_t *decl_specs,
                    ncc_parse_tree_t *declarator,
                    ncc_gc_stack_root_kind_t kind, ncc_parse_tree_t *scope,
                    ncc_parse_tree_t *decl_node)
{
    if (!decl_specs || !declarator) {
        return;
    }

    int  ptr_depth = count_leaf_text(declarator, "*")
                   + count_leaf_text(declarator, "^");
    bool is_array  = first_descendant_nt(declarator, "array_declarator")
                  != nullptr;
    bool aggregate = decl_specs_are_aggregate(decl_specs);

    if (kind == NCC_GC_STACK_ROOT_PARAM && is_array) {
        ptr_depth = ptr_depth > 0 ? ptr_depth : 1;
        is_array  = false;
    }

    if (ptr_depth == 0 && !is_array && !aggregate) {
        return;
    }

    char *name = declarator_name(declarator);
    if (!name) {
        gc_stack_errorf(declarator,
                        "GC stack-map root in function '%s' is unsupported: "
                        "pointer or aggregate parameter has no name",
                        function);
    }

    ncc_gc_stack_root_shape_t shape     = NCC_GC_STACK_ROOT_POINTER;
    uint64_t                  num_words = 1;

    if (is_array) {
        if (ptr_depth > 0 && has_parenthesized_pointer_array(declarator)) {
            gc_stack_errorf(declarator,
                            "GC stack-map root '%s' in function '%s' uses a "
                            "parenthesized pointer/array declarator; spell the "
                            "root as a direct fixed-size pointer array or a "
                            "single pointer",
                            name, function);
        }

        ncc_parse_tree_t *bad_array = nullptr;
        uint64_t          product   = 1;
        if (!array_bound_product(declarator, &product, &bad_array)) {
            gc_stack_errorf(bad_array ? bad_array : declarator,
                            "GC stack-map root '%s' in function '%s' uses a "
                            "variable-length or incomplete array; only "
                            "fixed-size stack roots are supported",
                            name, function);
        }

        num_words = product;
        shape     = ptr_depth > 0 ? NCC_GC_STACK_ROOT_POINTER_ARRAY
                                  : NCC_GC_STACK_ROOT_AGGREGATE;
    }
    else if (aggregate) {
        shape     = NCC_GC_STACK_ROOT_AGGREGATE;
        num_words = 0;
    }

    record_root(ctx, function, name, decl_specs, kind, shape, num_words,
                scope, decl_node);
    ncc_free(name);
}

static void
scan_parameter_node(ncc_xform_ctx_t *ctx, const char *function,
                    ncc_parse_tree_t *scope, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "parameter_declaration")) {
        ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
            node, "declaration_specifiers");
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            node, "declarator");
        if (!declarator) {
            declarator = ncc_xform_find_child_nt(node,
                                                 "abstract_declarator");
        }

        classify_declarator(ctx, function, decl_specs, declarator,
                            NCC_GC_STACK_ROOT_PARAM, scope, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_parameter_node(ctx, function, scope, ncc_tree_child(node, i));
    }
}

static void
scan_init_declarators(ncc_xform_ctx_t *ctx, const char *function,
                      ncc_parse_tree_t *decl_specs, ncc_parse_tree_t *scope,
                      ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "init_declarator")) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            node, "declarator");
        classify_declarator(ctx, function, decl_specs, declarator,
                            NCC_GC_STACK_ROOT_LOCAL, scope, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_init_declarators(ctx, function, decl_specs, scope,
                              ncc_tree_child(node, i));
    }
}

static void
scan_declaration(ncc_xform_ctx_t *ctx, const char *function,
                 ncc_parse_tree_t *decl)
{
    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        decl, "declaration_specifiers");
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!decl_specs || !list || !decl_is_stack_storage(decl_specs)) {
        return;
    }

    ncc_parse_tree_t *scope = ncc_xform_find_ancestor(decl,
                                                      "compound_statement");
    scan_init_declarators(ctx, function, decl_specs, scope, list);
}

static void
scan_compound(ncc_xform_ctx_t *ctx, const char *function,
              ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "declaration")) {
        scan_declaration(ctx, function, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_compound(ctx, function, ncc_tree_child(node, i));
    }
}

static ncc_parse_tree_t *
xform_gc_stack_function(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *fn)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->gc_stack_maps) {
        return nullptr;
    }

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(fn, "declarator");
    ncc_parse_tree_t *body       = ncc_xform_find_child_nt(fn,
                                                           "function_body");
    ncc_parse_tree_t *compound   = ncc_xform_find_child_nt(body,
                                                           "compound_statement");
    if (!declarator || !compound) {
        return nullptr;
    }

    char *fname = function_name(declarator);
    ncc_parse_tree_t *params = first_descendant_nt(declarator,
                                                   "parameter_type_list");
    if (params) {
        scan_parameter_node(ctx, fname, compound, params);
    }

    scan_compound(ctx, fname, compound);
    ncc_free(fname);
    return nullptr;
}

void
ncc_register_gc_stack_maps_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "function_definition", xform_gc_stack_function,
                       "gc_stack_maps");
}
