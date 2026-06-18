// xform_type_layout.c -- Shared C aggregate/pointer type-layout helpers.

#include "xform/xform_type_layout.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "parse/type_infer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

char *
ncc_layout_copy_cstr(const char *s)
{
    size_t len = s ? strlen(s) : 0;
    char  *r   = ncc_alloc_size(1, len + 1);
    if (len > 0) {
        memcpy(r, s, len);
    }
    r[len] = '\0';
    return r;
}

char *
ncc_layout_trim_copy(const char *s)
{
    if (!s) {
        return ncc_layout_copy_cstr("");
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

char *
ncc_layout_format_cstr(const char *fmt, ...)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;
    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    return ncc_buffer_take(buf);
}

char *
ncc_layout_node_text(ncc_parse_tree_t *node)
{
    ncc_string_t s = ncc_xform_node_to_text(node);
    char        *r = ncc_layout_trim_copy(s.data);
    ncc_free(s.data);
    return r;
}

void
ncc_layout_parse_tree_list_push(ncc_layout_parse_tree_list_t *list,
                                ncc_parse_tree_t *node)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(ncc_parse_tree_t *));
        list->cap = new_cap;
    }

    list->data[list->len++] = node;
}

void
ncc_layout_uint64_list_push(ncc_layout_uint64_list_t *list, uint64_t value)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 4;
        list->data = ncc_realloc(list->data, new_cap * sizeof(uint64_t));
        list->cap = new_cap;
    }

    list->data[list->len++] = value;
}

void
ncc_layout_collect_nt_children(ncc_parse_tree_t *node, const char *name,
                               ncc_layout_parse_tree_list_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        ncc_layout_parse_tree_list_push(out, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_layout_collect_nt_children(ncc_tree_child(node, i), name, out);
    }
}

ncc_parse_tree_t *
ncc_layout_first_descendant_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found =
            ncc_layout_first_descendant_nt(ncc_tree_child(node, i), name);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

ncc_token_info_t *
ncc_layout_first_leaf_token(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_tree_leaf_value(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *tok =
            ncc_layout_first_leaf_token(ncc_tree_child(node, i));
        if (tok) {
            return tok;
        }
    }

    return nullptr;
}

static const char *
layout_token_file(ncc_token_info_t *tok)
{
    if (!tok || !ncc_option_is_set(tok->file)) {
        return nullptr;
    }

    ncc_string_t file = ncc_option_get(tok->file);
    return file.data;
}

static bool
layout_token_file_looks_system_header(ncc_token_info_t *tok)
{
    const char *file = layout_token_file(tok);
    if (!file) {
        return false;
    }

    return strstr(file, "/usr/include/") != nullptr
        || strstr(file, "/usr/lib/clang/") != nullptr
        || strstr(file, "/System/Library/Frameworks/") != nullptr
        || strstr(file, "/opt/homebrew/") != nullptr
        || strstr(file, "/usr/local/") != nullptr;
}

bool
ncc_layout_node_starts_in_system_header(ncc_parse_tree_t *node)
{
    ncc_token_info_t *tok = ncc_layout_first_leaf_token(node);
    return tok && (tok->system_header
                   || layout_token_file_looks_system_header(tok));
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

static bool
node_direct_child_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_xform_leaf_text_eq(ncc_tree_child(node, i), text)) {
            return true;
        }
    }

    return false;
}

static int
pointer_depth_in_type_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return 0;
    }

    if (ncc_xform_nt_name_is(node, "parameter_type_list")
        || ncc_xform_nt_name_is(node, "assignment_expression")) {
        return 0;
    }

    int count = 0;
    if (ncc_xform_nt_name_is(node, "pointer")) {
        count += node_direct_child_leaf_text(node, "*") ? 1 : 0;
        count += node_direct_child_leaf_text(node, "^") ? 1 : 0;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += pointer_depth_in_type_node(ncc_tree_child(node, i));
    }

    return count;
}

int
ncc_layout_pointer_depth_in_declarator(ncc_parse_tree_t *node)
{
    return pointer_depth_in_type_node(node);
}

bool
ncc_layout_declarator_is_function_pointer(ncc_parse_tree_t *declarator)
{
    if (!declarator) {
        return false;
    }

    return ncc_layout_first_descendant_nt(declarator, "parameter_type_list")
        != nullptr
        || ncc_layout_first_descendant_nt(declarator, "parameter_list")
               != nullptr
        || ncc_layout_first_descendant_nt(declarator, "identifier_list")
               != nullptr;
}

static int
pointer_depth_in_decl_specs(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return 0;
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return 0;
    }

    int count = 0;
    if (ncc_xform_nt_name_is(node, "pointer")) {
        count += node_direct_child_leaf_text(node, "*") ? 1 : 0;
        count += node_direct_child_leaf_text(node, "^") ? 1 : 0;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += pointer_depth_in_decl_specs(ncc_tree_child(node, i));
    }

    return count;
}

char *
ncc_layout_declarator_name(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (!ncc_tree_is_leaf(node)
        && (ncc_xform_nt_name_is(node, "parameter_type_list")
            || ncc_xform_nt_name_is(node, "assignment_expression"))) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (text
            && (strcmp(text, "const") == 0
                || strcmp(text, "restrict") == 0
                || strcmp(text, "volatile") == 0
                || strcmp(text, "_Atomic") == 0
                || strcmp(text, "_Nullable") == 0
                || strcmp(text, "_Nonnull") == 0
                || strcmp(text, "_Null_unspecified") == 0
                || strcmp(text, "__const__") == 0
                || strcmp(text, "__const") == 0
                || strcmp(text, "__restrict__") == 0
                || strcmp(text, "__restrict") == 0
                || strcmp(text, "__volatile__") == 0
                || strcmp(text, "__volatile") == 0
                || strcmp(text, "__nullable") == 0
                || strcmp(text, "__nonnull") == 0
                || strcmp(text, "__null_unspecified") == 0)) {
            return nullptr;
        }
        if (text && (isalpha((unsigned char)text[0]) || text[0] == '_')) {
            return ncc_layout_copy_cstr(text);
        }
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = ncc_layout_declarator_name(ncc_tree_child(node, i));
        if (candidate) {
            return candidate;
        }
    }
    return nullptr;
}

bool
ncc_layout_parse_positive_integer_literal(const char *text, uint64_t *out)
{
    char *trimmed = ncc_layout_trim_copy(text);
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
array_bound_allows_static_sizeof(const char *text)
{
    char *trimmed = ncc_layout_trim_copy(text);
    bool  saw_upper = false;
    bool  ok        = trimmed[0] != '\0';

    for (char *p = trimmed; ok && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isupper(c) || *p == '_') {
            saw_upper = true;
            continue;
        }
        if (isdigit(c) || isspace(c) || *p == '(' || *p == ')' || *p == '+'
            || *p == '-' || *p == '*' || *p == '/' || *p == '%'
            || *p == '<' || *p == '>' || *p == '|' || *p == '&'
            || *p == '~') {
            continue;
        }
        ok = false;
    }

    ncc_free(trimmed);
    return ok && saw_upper;
}

static bool
array_bound_product(ncc_parse_tree_t *node, uint64_t *product,
                    bool *use_sizeof, ncc_parse_tree_t **bad_array)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return true;
    }

    if (ncc_xform_nt_name_is(node, "array_declarator")) {
        ncc_parse_tree_t *bound = ncc_xform_find_child_nt(
            node, "assignment_expression");
        uint64_t count = 0;

        if (!bound) {
            *use_sizeof = true;
        }
        else {
            char *text = ncc_layout_node_text(bound);
            bool  ok   = ncc_layout_parse_positive_integer_literal(text,
                                                                    &count);
            if (ok) {
                *product *= count;
            }
            else if (array_bound_allows_static_sizeof(text)) {
                *use_sizeof = true;
            }
            else {
                ncc_free(text);
                *bad_array = node;
                return false;
            }
            ncc_free(text);
        }
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (!array_bound_product(ncc_tree_child(node, i), product,
                                 use_sizeof, bad_array)) {
            return false;
        }
    }

    return true;
}

bool
ncc_layout_has_unbounded_array_declarator(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    if (ncc_xform_nt_name_is(node, "array_declarator")
        && ncc_xform_find_child_nt(node, "assignment_expression") == nullptr) {
        return true;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_layout_has_unbounded_array_declarator(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

bool
ncc_layout_array_declarator_dimensions(ncc_parse_tree_t *declarator,
                                       ncc_layout_uint64_list_t *dims,
                                       ncc_parse_tree_t **bad_array)
{
    char *text = ncc_layout_node_text(declarator);
    char *p    = text;

    while ((p = strchr(p, '[')) != nullptr) {
        char *close = strchr(p + 1, ']');
        if (!close) {
            *bad_array = declarator;
            ncc_free(text);
            return false;
        }

        size_t len = (size_t)(close - (p + 1));
        char  *bound = ncc_alloc_size(1, len + 1);
        memcpy(bound, p + 1, len);
        bound[len] = '\0';

        uint64_t count = 0;
        bool ok = ncc_layout_parse_positive_integer_literal(bound, &count);
        ncc_free(bound);
        if (!ok) {
            *bad_array = declarator;
            ncc_free(text);
            return false;
        }

        ncc_layout_uint64_list_push(dims, count);
        p = close + 1;
    }

    ncc_free(text);
    return dims->len > 0;
}

static bool
is_group_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$';
}

static uint64_t
count_top_level_initializers(ncc_parse_tree_t *list)
{
    if (!list || ncc_tree_is_leaf(list)) {
        return 0;
    }

    uint64_t count = 0;
    size_t   nc    = ncc_tree_num_children(list);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(list, i);
        if (ncc_xform_nt_name_is(child, "initializer")) {
            count++;
        }
        else if (is_group_node(child)) {
            count += count_top_level_initializers(child);
        }
    }

    return count;
}

static bool
infer_array_bound_from_initializer(ncc_parse_tree_t *decl_node,
                                   uint64_t *count_out)
{
    ncc_parse_tree_t *initializer = ncc_xform_find_child_nt(decl_node,
                                                            "initializer");
    ncc_parse_tree_t *braced = ncc_xform_find_child_nt(initializer,
                                                       "braced_initializer");
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(braced,
                                                     "initializer_list");
    uint64_t count = count_top_level_initializers(list);
    if (count == 0) {
        return false;
    }

    *count_out = count;
    return true;
}

bool
ncc_layout_array_literal_dimensions(ncc_parse_tree_t *declarator,
                                    ncc_parse_tree_t *decl_node,
                                    ncc_layout_uint64_list_t *dims)
{
    char *text = ncc_layout_node_text(declarator);
    char *p    = text;

    while ((p = strchr(p, '[')) != nullptr) {
        char *close = strchr(p + 1, ']');
        if (!close) {
            ncc_free(text);
            return false;
        }

        size_t len = (size_t)(close - (p + 1));
        char  *bound = ncc_alloc_size(1, len + 1);
        memcpy(bound, p + 1, len);
        bound[len] = '\0';

        uint64_t count = 0;
        bool ok = false;
        char *trimmed = ncc_layout_trim_copy(bound);
        if (trimmed[0] == '\0' && dims->len == 0) {
            ok = infer_array_bound_from_initializer(decl_node, &count);
        }
        else {
            ok = ncc_layout_parse_positive_integer_literal(trimmed, &count);
        }
        ncc_free(trimmed);
        ncc_free(bound);
        if (!ok) {
            ncc_free(text);
            return false;
        }

        ncc_layout_uint64_list_push(dims, count);
        p = close + 1;
    }

    ncc_free(text);
    return dims->len > 0;
}

char *
ncc_layout_num_words_expr_for_pointer_array(ncc_parse_tree_t *declarator,
                                            const char *expr,
                                            ncc_parse_tree_t **bad_array)
{
    uint64_t product    = 1;
    bool     use_sizeof = false;
    if (!array_bound_product(declarator, &product, &use_sizeof, bad_array)) {
        return nullptr;
    }

    if (use_sizeof) {
        return ncc_layout_format_cstr("(sizeof(%s)/sizeof(void *))", expr);
    }

    return ncc_layout_format_cstr("%llu", (unsigned long long)product);
}

static ncc_dict_t *
layout_aggregate_types(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_aggregate_types : nullptr;
}

static ncc_dict_t *
layout_pointer_typedefs(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_pointer_typedefs : nullptr;
}

static ncc_dict_t *
layout_function_pointer_typedefs(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_function_pointer_typedefs : nullptr;
}

static void
record_function_pointer_typedef(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_dict_t *fns = layout_function_pointer_typedefs(ctx);
    if (!fns || !name || !*name) {
        return;
    }
    bool found = false;
    ncc_dict_get(fns, (void *)name, &found);
    if (!found) {
        ncc_dict_put(fns, ncc_layout_copy_cstr(name), (void *)(uintptr_t)1);
    }
}

bool
ncc_layout_typedef_name_is_function_pointer(ncc_xform_ctx_t *ctx,
                                            const char *name)
{
    ncc_dict_t *fns = layout_function_pointer_typedefs(ctx);
    if (!fns || !name) {
        return false;
    }
    bool found = false;
    (void)ncc_dict_get(fns, (void *)name, &found);
    return found;
}

static char *
struct_or_union_kind_text(ncc_parse_tree_t *su)
{
    // A specifier's text begins with its own keyword ("struct"/"union"). A
    // descendant keyword search would, for a struct whose first member is an
    // anonymous union, wrongly return that nested "union" and misclassify the
    // outer struct as a union. Use the leading keyword of the specifier text.
    // The specifier's first leaf token is its own keyword ("struct"/"union").
    ncc_token_info_t *tok = ncc_layout_first_leaf_token(su);
    if (tok && ncc_option_is_set(tok->value)) {
        ncc_string_t v = ncc_option_get(tok->value);
        if (v.data && strcmp(v.data, "union") == 0) {
            return ncc_layout_copy_cstr("union");
        }
        if (v.data && strcmp(v.data, "struct") == 0) {
            return ncc_layout_copy_cstr("struct");
        }
    }
    return ncc_layout_copy_cstr("struct");
}

bool
ncc_layout_struct_or_union_is_union(ncc_parse_tree_t *su)
{
    char *kind = struct_or_union_kind_text(su);
    bool  r    = kind && strcmp(kind, "union") == 0;
    ncc_free(kind);
    return r;
}

char *
ncc_layout_aggregate_key_from_specifier(ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
    if (!tag) {
        return nullptr;
    }

    char *kind = struct_or_union_kind_text(su);
    char *name = ncc_layout_node_text(tag);
    if (!kind || !name || !*name) {
        ncc_free(kind);
        ncc_free(name);
        return nullptr;
    }

    char *key = ncc_layout_format_cstr("%s %s", kind, name);
    ncc_free(kind);
    ncc_free(name);
    return key;
}

bool
ncc_layout_aggregate_specifier_has_members(ncc_parse_tree_t *su)
{
    return su
        && ncc_xform_find_child_nt(su, "member_declaration_list") != nullptr;
}

static void
record_aggregate_type(ncc_xform_ctx_t *ctx, const char *key,
                      ncc_parse_tree_t *specifier, bool is_atomic,
                      const char *offset_type);

ncc_layout_aggregate_type_info_t *
ncc_layout_lookup_aggregate_type(ncc_xform_ctx_t *ctx, const char *key)
{
    ncc_dict_t *types = layout_aggregate_types(ctx);
    if (!types || !key) {
        return nullptr;
    }

    bool found = false;
    ncc_layout_aggregate_type_info_t *info =
        ncc_dict_get(types, (void *)key, &found);
    if (found) {
        return info;
    }

    // Fall back to the symbol table — the single source of truth for types.
    // The eager collector populates `types` from declarations as it sees them,
    // but misses forms it cannot resolve at collection time (notably
    // _generic_struct typedefs: the real n00b_variant_t / generic-container
    // shape). The symbol table resolves those; cache the result so later
    // lookups hit the dict. The resolver returns a spec only for a genuine
    // aggregate (it yields null for pointer typedefs), so this never turns a
    // pointer into a by-value aggregate.
    ncc_symtab_t *st = ncc_xform_get_data(ctx)->symtab;
    ncc_parse_tree_t *spec = st ? ncc_symtab_aggregate_spec(st, key) : nullptr;
    // The symbol table records system-header tags too (it has no system gate),
    // but layout/GC passes must NOT resolve a system aggregate to its definition:
    // the eager collector deliberately excluded system-header definitions
    // (!node_starts_in_system_header), so those types were scanned conservatively
    // rather than descended into. Descending breaks on system shapes the GC walker
    // can't address — e.g. `struct sigaction`, whose handler pointers live in an
    // anonymous union reached only through a macro, yielding bogus member paths
    // (`sa.__sa_handler`). Preserve that policy here so the symtab is the single
    // source of truth WITHOUT widening which types get descended.
    if (spec && !ncc_layout_node_starts_in_system_header(spec)) {
        record_aggregate_type(ctx, key, spec, false, key);
        info = ncc_dict_get(types, (void *)key, &found);
        return found ? info : nullptr;
    }
    return nullptr;
}

static void
record_aggregate_type(ncc_xform_ctx_t *ctx, const char *key,
                      ncc_parse_tree_t *specifier, bool is_atomic,
                      const char *offset_type)
{
    ncc_dict_t *types = layout_aggregate_types(ctx);
    if (!types || !key || !*key || !specifier) {
        return;
    }

    bool found = false;
    ncc_dict_get(types, (void *)key, &found);
    if (found) {
        return;
    }

    ncc_layout_aggregate_type_info_t *info =
        ncc_alloc(ncc_layout_aggregate_type_info_t);
    info->key         = ncc_layout_copy_cstr(key);
    info->specifier   = specifier;
    info->offset_type = ncc_layout_copy_cstr(offset_type ? offset_type : key);
    info->is_atomic   = is_atomic;
    info->is_union    = ncc_layout_struct_or_union_is_union(specifier);
    ncc_dict_put(types, info->key, info);
}

ncc_parse_tree_t *
ncc_layout_resolve_aggregate_specifier(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *su)
{
    if (!su) {
        return nullptr;
    }

    if (ncc_layout_aggregate_specifier_has_members(su)) {
        return su;
    }

    char *key = ncc_layout_aggregate_key_from_specifier(su);
    ncc_layout_aggregate_type_info_t *info =
        ncc_layout_lookup_aggregate_type(ctx, key);
    ncc_free(key);
    return info ? info->specifier : nullptr;
}

char *
ncc_layout_first_typedef_name_text(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "typedef_name")) {
        return ncc_layout_node_text(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate =
            ncc_layout_first_typedef_name_text(ncc_tree_child(node, i));
        if (candidate) {
            return candidate;
        }
    }

    return nullptr;
}

bool
ncc_layout_typedef_name_is_pointer(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_dict_t *ptrs = layout_pointer_typedefs(ctx);
    if (!ptrs || !name) {
        return false;
    }

    bool found = false;
    (void)ncc_dict_get(ptrs, (void *)name, &found);
    return found;
}

static char *
first_known_type_alias_text(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node,
                            bool want_pointer, bool want_aggregate)
{
    if (!node) {
        return nullptr;
    }

    if (!ncc_tree_is_leaf(node)
        && ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (!text || !is_ident_start_char(text[0])) {
            return nullptr;
        }

        if (want_pointer && ncc_layout_typedef_name_is_pointer(ctx, text)) {
            return ncc_layout_copy_cstr(text);
        }

        if (want_aggregate && ncc_layout_lookup_aggregate_type(ctx, text)) {
            return ncc_layout_copy_cstr(text);
        }

        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = first_known_type_alias_text(
            ctx, ncc_tree_child(node, i), want_pointer, want_aggregate);
        if (candidate) {
            return candidate;
        }
    }

    return nullptr;
}

static bool
atomic_type_specifier_is_pointer(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return false;
    }

    if (ncc_xform_nt_name_is(node, "atomic_type_specifier")) {
        if (ncc_layout_first_descendant_nt(node, "pointer") != nullptr) {
            return true;
        }

        char *text = ncc_layout_node_text(node);
        bool  result = text && strstr(text, "_Atomic") != nullptr
                    && strchr(text, '*') != nullptr;
        if (text) {
            ncc_free(text);
        }
        return result;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (atomic_type_specifier_is_pointer(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

ncc_parse_tree_t *
ncc_layout_aggregate_spec_from_specs(ncc_xform_ctx_t *ctx,
                                     ncc_parse_tree_t *specs)
{
    // An `_Atomic(T *)` member is a POINTER (the `*` is in the atomic wrapper,
    // outside any aggregate body), not a by-value aggregate — even though its
    // specs contain T's struct specifier. Reject it here so the caller emits a
    // pointer rather than recursing and emitting offsetof through the pointer.
    if (atomic_type_specifier_is_pointer(specs)) {
        return nullptr;
    }

    // An INLINE struct/union/_generic_struct specifier is a by-value aggregate;
    // resolve it directly. This must come BEFORE the pointer test:
    // pointer_depth_for_specs sees pointer MEMBERS in the inline body (e.g. the
    // `T *data` of an inline n00b_list_t) and would wrongly report depth > 0,
    // dropping the aggregate. A member that is genuinely a pointer carries its
    // `*` in the declarator (handled by the caller), not in these specifiers,
    // and a pointer typedef has no inline specifier (the typedef path below).
    ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
        specs, "struct_or_union_specifier");
    if (su) {
        return ncc_layout_resolve_aggregate_specifier(ctx, su);
    }

    if (ncc_layout_pointer_depth_for_specs(ctx, specs) > 0) {
        return nullptr;
    }

    char *td_name = ncc_layout_first_typedef_name_text(specs);
    ncc_layout_aggregate_type_info_t *info =
        ncc_layout_lookup_aggregate_type(ctx, td_name);
    if (!info && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, false, true);
        info    = ncc_layout_lookup_aggregate_type(ctx, td_name);
    }
    ncc_free(td_name);
    return info ? info->specifier : nullptr;
}

ncc_layout_aggregate_type_info_t *
ncc_layout_aggregate_info_from_specs(ncc_xform_ctx_t *ctx,
                                     ncc_parse_tree_t *specs)
{
    if (ncc_layout_pointer_depth_for_specs(ctx, specs) > 0) {
        return nullptr;
    }

    ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
        specs, "struct_or_union_specifier");
    if (su) {
        char *key = ncc_layout_aggregate_key_from_specifier(su);
        ncc_layout_aggregate_type_info_t *info =
            ncc_layout_lookup_aggregate_type(ctx, key);
        ncc_free(key);
        if (info) {
            return info;
        }
        if (ncc_layout_aggregate_specifier_has_members(su)) {
            return nullptr;
        }
    }

    char *td_name = ncc_layout_first_typedef_name_text(specs);
    ncc_layout_aggregate_type_info_t *info =
        ncc_layout_lookup_aggregate_type(ctx, td_name);
    if (!info && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, false, true);
        info    = ncc_layout_lookup_aggregate_type(ctx, td_name);
    }
    ncc_free(td_name);
    return info;
}

ncc_layout_aggregate_type_info_t *
ncc_layout_aggregate_info_from_type_name(ncc_xform_ctx_t *ctx,
                                         const char *type_name)
{
    char *trimmed = ncc_layout_trim_copy(type_name);
    ncc_layout_aggregate_type_info_t *info =
        ncc_layout_lookup_aggregate_type(ctx, trimmed);
    ncc_free(trimmed);
    return info;
}

bool
ncc_layout_specs_have_atomic_type_wrapper(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, "_Atomic");
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_layout_specs_have_atomic_type_wrapper(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

char *
ncc_layout_aggregate_offset_type_from_specs(ncc_xform_ctx_t *ctx,
                                            ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
        specs, "struct_or_union_specifier");
    if (su) {
        char *key = ncc_layout_aggregate_key_from_specifier(su);
        if (key) {
            return key;
        }
    }

    char *td_name = ncc_layout_first_typedef_name_text(specs);
    if (td_name) {
        return td_name;
    }

    return first_known_type_alias_text(ctx, specs, false, true);
}

static bool
specs_are_unnamed_inline_aggregate(ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
        specs, "struct_or_union_specifier");
    return su && ncc_layout_aggregate_specifier_has_members(su)
        && ncc_xform_find_child_nt(su, "tag_name") == nullptr;
}

char *
ncc_layout_implicit_member_field_name(ncc_parse_tree_t *member,
                                      ncc_parse_tree_t *member_specs)
{
    char  *text = ncc_layout_node_text(member);
    size_t end  = strlen(text);
    char  *semi = strrchr(text, ';');
    if (semi) {
        end = (size_t)(semi - text);
    }

    if (specs_are_unnamed_inline_aggregate(member_specs)) {
        char *close = nullptr;
        for (size_t i = 0; i < end; i++) {
            if (text[i] == '}') {
                close = text + i;
            }
        }

        if (!close || (size_t)(close - text + 1) >= end) {
            ncc_free(text);
            return nullptr;
        }

        size_t start = (size_t)(close - text + 1);
        size_t len   = end - start;
        char  *tail  = ncc_alloc_size(1, len + 1);
        memcpy(tail, text + start, len);
        tail[len] = '\0';
        char *name = copy_last_identifier_before(tail, len);
        ncc_free(tail);
        ncc_free(text);
        return name;
    }

    char *name = copy_last_identifier_before(text, end);
    ncc_free(text);
    if (!name) {
        return nullptr;
    }

    ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
        member_specs, "struct_or_union_specifier");
    char *td_name = ncc_layout_first_typedef_name_text(member_specs);
    if (!su && td_name && strcmp(td_name, name) == 0) {
        ncc_free(td_name);
        ncc_free(name);
        return nullptr;
    }
    ncc_free(td_name);

    ncc_parse_tree_t *tag = su ? ncc_xform_find_child_nt(su, "tag_name")
                               : nullptr;
    if (tag) {
        char *tag_name = ncc_layout_node_text(tag);
        if (tag_name && strcmp(tag_name, name) == 0) {
            ncc_free(tag_name);
            ncc_free(name);
            return nullptr;
        }
        ncc_free(tag_name);
    }

    return name;
}

static bool
decl_specs_use_pointer_typedef(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    char *td_name = ncc_layout_first_typedef_name_text(specs);
    bool  result  = ncc_layout_typedef_name_is_pointer(ctx, td_name);
    if (!result && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, true, false);
        result  = ncc_layout_typedef_name_is_pointer(ctx, td_name);
    }
    ncc_free(td_name);
    return result;
}

int
ncc_layout_pointer_depth_for_specs(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *specs)
{
    int ptr_depth = pointer_depth_in_decl_specs(specs);
    if (decl_specs_use_pointer_typedef(ctx, specs)) {
        ptr_depth++;
    }
    // A pointer can hide inside an _Atomic(T *) wrapper, where the '*' is an
    // abstract declarator (a `pointer` NT) within the atomic_type_specifier
    // that the scans above don't reach. Count it so every consumer classifies
    // _Atomic(T *) members/typedefs as pointers (not by-value aggregates).
    if (ptr_depth == 0 && atomic_type_specifier_is_pointer(specs)) {
        ptr_depth++;
    }
    return ptr_depth;
}

int
ncc_layout_pointer_depth_for_declarator(ncc_xform_ctx_t *ctx,
                                        ncc_parse_tree_t *specs,
                                        ncc_parse_tree_t *declarator)
{
    return ncc_layout_pointer_depth_in_declarator(declarator)
         + ncc_layout_pointer_depth_for_specs(ctx, specs);
}

static void
record_pointer_typedef(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_dict_t *ptrs = layout_pointer_typedefs(ctx);
    if (!ptrs || !name || !*name) {
        return;
    }

    bool found = false;
    ncc_dict_get(ptrs, (void *)name, &found);
    if (!found) {
        ncc_dict_put(ptrs, ncc_layout_copy_cstr(name), (void *)(uintptr_t)1);
    }
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

static void
collect_typedef_aliases_from_decl(ncc_xform_ctx_t *ctx,
                                  ncc_parse_tree_t *decl)
{
    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        decl, "declaration_specifiers");
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!decl_specs || !list || !contains_leaf_text(decl_specs, "typedef")) {
        return;
    }

    ncc_layout_parse_tree_list_t inits = {0};
    ncc_layout_collect_nt_children(list, "init_declarator", &inits);

    for (size_t i = 0; i < inits.len; i++) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            inits.data[i], "declarator");
        char *name = ncc_layout_declarator_name(declarator);
        if (!name) {
            continue;
        }

        // An _Atomic(T *) typedef ( typedef _Atomic(struct X *) name; ) hides
        // its '*' in an abstract declarator inside the atomic_type_specifier,
        // so the declarator/specs pointer checks miss it. Detect it here so
        // the typedef is classified as a POINTER (not an aggregate alias) —
        // otherwise downstream code treats fields of this type as by-value
        // structs.
        bool atomic_ptr = atomic_type_specifier_is_pointer(decl_specs);

        if (ncc_layout_pointer_depth_in_declarator(declarator) > 0
            || decl_specs_use_pointer_typedef(ctx, decl_specs)
            || atomic_ptr) {
            record_pointer_typedef(ctx, name);
            // A function-pointer typedef ( typedef RET (*name)(params) ) has a
            // parameter list inside its declarator. Record it separately so
            // the GC-map walker can exclude it (code pointers are not heap
            // pointers and must not be scanned/marshaled as data).
            if (ncc_layout_first_descendant_nt(declarator, "parameter_type_list")
                || ncc_layout_first_descendant_nt(declarator, "parameter_list")) {
                record_function_pointer_typedef(ctx, name);
            }
            ncc_free(name);
            continue;
        }

        ncc_parse_tree_t *aggregate = ncc_layout_aggregate_spec_from_specs(
            ctx, decl_specs);
        if (aggregate) {
            bool  is_atomic = ncc_layout_specs_have_atomic_type_wrapper(
                decl_specs);
            char *offset_type =
                is_atomic
                    ? ncc_layout_aggregate_offset_type_from_specs(ctx,
                                                                  decl_specs)
                    : nullptr;
            record_aggregate_type(ctx, name, aggregate, is_atomic,
                                  offset_type ? offset_type : name);
            ncc_free(offset_type);
        }

        ncc_free(name);
    }

    ncc_free(inits.data);
}

static void
collect_typedef_aliases(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "declaration")
        && !ncc_layout_node_starts_in_system_header(node)) {
        collect_typedef_aliases_from_decl(ctx, node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_typedef_aliases(ctx, ncc_tree_child(node, i));
    }
}

void
ncc_layout_collect_type_info(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu)
{
    // The eager struct/union *definition* walk is retired: the symbol table is
    // now the single source of truth for aggregate-tag resolution. Every pass
    // resolves through ncc_layout_lookup_aggregate_type, which falls back to the
    // symtab (and caches) when the legacy dict misses, so a standalone
    // `struct X { ... }` tag no longer needs to be pre-recorded here.
    //
    // The typedef-alias walk stays: it performs the load-bearing pointer- and
    // function-pointer-typedef classification (and records aggregate *aliases*
    // with their precise offset_type/is_atomic info) that the symtab fallback
    // does not reproduce.
    collect_typedef_aliases(ctx, tu);
}
