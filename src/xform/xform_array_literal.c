// xform_array_literal.c -- Transform: ncc/n00b array literal initializers.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_rstr.h"
#include "xform/xform_static_object.h"
#include "xform/xform_type_layout.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *object_type;
    char *elem_type;
} array_type_info_t;

typedef struct {
    char **items;
    int    count;
    int    cap;
} string_list_t;

typedef struct {
    const char *scan_kind;
    const char *scan_cb;
    const char *scan_user;
    const char *no_scan;
    char        ptr_words[32];
    char        shape_name[64];
    char        stride_words[128];
    char       *shape_decl;
    char       *owned_scan_user;
} array_scan_plan_t;

typedef ncc_layout_aggregate_type_info_t array_aggregate_type_info_t;
typedef ncc_layout_uint64_list_t uint64_list_t;

static void array_errorf(ncc_parse_tree_t *node, const char *fmt, ...);

static ncc_dict_t *
array_types(ncc_xform_ctx_t *ctx)
{
    return &ncc_xform_get_data(ctx)->array_types;
}

static const char *
get_array_literal_data_template(ncc_xform_ctx_t *ctx)
{
    return ncc_xform_get_data(ctx)->array_literal_data_template;
}

static const char *
get_array_literal_data_expr(ncc_xform_ctx_t *ctx)
{
    return ncc_xform_get_data(ctx)->array_literal_data_expr;
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

static void
parse_tree_array_push(ncc_array_t(ncc_parse_tree_ptr_t) *array,
                      ncc_parse_tree_t *node)
{
    if (array->len >= array->cap) {
        size_t new_cap = array->cap ? array->cap * 2 : 8;
        array->data = ncc_realloc(array->data,
                                  new_cap * sizeof(*array->data));
        array->cap = new_cap;
    }

    ncc_array_set(*array, array->len, node);
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

static void
list_push(string_list_t *list, char *s)
{
    if (list->count >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 8;
        list->items = ncc_realloc(list->items,
                                  (size_t)new_cap * sizeof(char *));
        list->cap = new_cap;
    }

    list->items[list->count++] = s;
}

static void
list_free(string_list_t *list)
{
    for (int i = 0; i < list->count; i++) {
        ncc_free(list->items[i]);
    }
    ncc_free(list->items);
    *list = (string_list_t){0};
}

static void
uint64_list_push(uint64_list_t *list, uint64_t value)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 4;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(*list->data));
        list->cap = new_cap;
    }

    list->data[list->len++] = value;
}

static void
record_array_type(ncc_xform_ctx_t *ctx, const char *key,
                  const char *object_type, const char *elem_type)
{
    if (!key || !*key || !object_type || !*object_type || !elem_type
        || !*elem_type) {
        return;
    }

    bool found = false;
    ncc_dict_get(array_types(ctx), (void *)key, &found);
    if (found) {
        return;
    }

    array_type_info_t *info = ncc_alloc(array_type_info_t);
    info->object_type       = copy_cstr(object_type);
    info->elem_type         = copy_cstr(elem_type);
    ncc_dict_put(array_types(ctx), copy_cstr(key), info);
}

static array_type_info_t *
lookup_array_type(ncc_xform_ctx_t *ctx, const char *key)
{
    if (!key || !*key) {
        return nullptr;
    }

    bool               found = false;
    array_type_info_t *info  = ncc_dict_get(array_types(ctx), (void *)key,
                                            &found);
    return found ? info : nullptr;
}

static char *
tag_runtime_name(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tag_node)
{
    ncc_parse_tree_t *synthetic = first_descendant_nt(tag_node,
                                                      "synthetic_identifier");
    if (synthetic && contains_leaf_text(synthetic, "typeid")) {
        ncc_parse_tree_t *atom = ncc_xform_find_child_nt(synthetic,
                                                         "typeid_atom");
        ncc_parse_tree_t *cont = ncc_xform_find_child_nt(
            synthetic, "typeid_continuation");
        if (atom) {
            ncc_string_t type_str = ncc_xform_extract_type_string(ctx, atom,
                                                                  cont);
            ncc_string_t mangled = ncc_type_mangle(type_str.data);
            char        *result  = copy_cstr(mangled.data);
            ncc_free(type_str.data);
            ncc_free(mangled.data);
            return result;
        }
    }

    return node_text(tag_node);
}

static bool
tag_is_array_type(ncc_parse_tree_t *tag_node)
{
    char *tag = node_text(tag_node);
    bool  result = false;

    if (strncmp(tag, "ncc_array_", 10) == 0) {
        result = true;
    }
    else if (strstr(tag, "typeid") && strstr(tag, "\"array\"")) {
        result = true;
    }

    ncc_free(tag);
    return result;
}

static char *
type_from_specifier_qualifier_list(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *sql)
{
    ncc_parse_tree_t *su = first_descendant_nt(sql, "struct_or_union_specifier");
    if (su) {
        ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
        if (tag) {
            char *runtime_tag = tag_runtime_name(ctx, tag);
            char *result      = ncc_alloc_size(1, strlen(runtime_tag) + 8);
            sprintf(result, "struct %s", runtime_tag);
            ncc_free(runtime_tag);
            return result;
        }
    }

    return node_text(sql);
}

static char *
append_pointer_stars(char *base, int stars)
{
    if (stars <= 0) {
        return base;
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_puts(buf, base);
    for (int i = 0; i < stars; i++) {
        ncc_buffer_puts(buf, " *");
    }
    ncc_free(base);
    return ncc_buffer_take(buf);
}

static ncc_parse_tree_t *
first_member_declaration(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (ncc_xform_nt_name_is(child, "member_declaration")) {
            return child;
        }
        if (is_group_node(child)) {
            ncc_parse_tree_t *found = first_member_declaration(child);
            if (found) {
                return found;
            }
        }
    }

    return nullptr;
}

static char *
array_elem_type_from_struct(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        su, "member_declaration_list");
    if (!members) {
        return nullptr;
    }

    ncc_parse_tree_t *member = first_member_declaration(members);
    if (!member || !contains_leaf_text(member, "data")) {
        return nullptr;
    }

    ncc_parse_tree_t *sql  = first_descendant_nt(member,
                                                 "specifier_qualifier_list");
    ncc_parse_tree_t *decl = first_descendant_nt(member, "declarator");
    if (!sql || !decl) {
        return nullptr;
    }

    int ptr_depth = count_leaf_text(decl, "*");
    if (ptr_depth < 1) {
        return nullptr;
    }

    char *base = type_from_specifier_qualifier_list(ctx, sql);
    return append_pointer_stars(base, ptr_depth - 1);
}

static array_type_info_t *
array_type_from_struct_spec(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
    if (!tag) {
        return nullptr;
    }

    char *runtime_tag = tag_runtime_name(ctx, tag);

    ncc_buffer_t *obj_buf = ncc_buffer_empty();
    ncc_buffer_printf(obj_buf, "struct %s", runtime_tag);
    char *object_type = ncc_buffer_take(obj_buf);

    char *elem_type = array_elem_type_from_struct(ctx, su);
    bool  is_array  = elem_type && tag_is_array_type(tag);

    if (!is_array) {
        array_type_info_t *known = lookup_array_type(ctx, object_type);
        ncc_free(runtime_tag);
        ncc_free(object_type);
        ncc_free(elem_type);
        return known;
    }

    record_array_type(ctx, object_type, object_type, elem_type);
    record_array_type(ctx, runtime_tag, object_type, elem_type);

    array_type_info_t *result = lookup_array_type(ctx, object_type);
    ncc_free(runtime_tag);
    ncc_free(object_type);
    ncc_free(elem_type);
    return result;
}

static char *
decl_specs_typedef_name(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (!text) {
            return nullptr;
        }

        static const char *skip[] = {
            "const", "volatile", "restrict", "static", "extern", "typedef",
            "auto", "register", "thread_local", "_Thread_local", "inline",
            nullptr,
        };

        for (int i = 0; skip[i]; i++) {
            if (strcmp(text, skip[i]) == 0) {
                return nullptr;
            }
        }

        if (isalpha((unsigned char)text[0]) || text[0] == '_') {
            return copy_cstr(text);
        }
        return nullptr;
    }

    char  *last = nullptr;
    size_t nc   = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = decl_specs_typedef_name(ncc_tree_child(node, i));
        if (candidate) {
            ncc_free(last);
            last = candidate;
        }
    }

    return last;
}

static array_type_info_t *
array_type_from_decl_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl_specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(decl_specs,
                                               "struct_or_union_specifier");
    if (su) {
        array_type_info_t *info = array_type_from_struct_spec(ctx, su);
        if (info) {
            return info;
        }
    }

    char *name = decl_specs_typedef_name(decl_specs);
    array_type_info_t *info = lookup_array_type(ctx, name);
    ncc_free(name);
    return info;
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

static void
collect_nt_children(ncc_parse_tree_t *node, const char *name,
                    ncc_array_t(ncc_parse_tree_ptr_t) *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (ncc_xform_nt_name_is(child, name)) {
            parse_tree_array_push(out, child);
            continue;
        }
        if (is_group_node(child)) {
            collect_nt_children(child, name, out);
        }
    }
}

static ncc_parse_tree_t *
initializer_array_literal(ncc_parse_tree_t *initializer)
{
    if (!initializer) {
        return nullptr;
    }
    return ncc_xform_find_child_nt(initializer, "array_literal");
}

static void
replace_initializer(ncc_parse_tree_t *node, ncc_parse_tree_t *replacement)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (ncc_xform_nt_name_is(child, "initializer")) {
            ncc_xform_set_child(node, i, replacement);
            return;
        }
        if (is_group_node(child)) {
            replace_initializer(child, replacement);
        }
    }
}

static void
collect_array_elements(ncc_parse_tree_t *array_lit,
                       ncc_array_t(ncc_parse_tree_ptr_t) *out)
{
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(array_lit,
                                                     "array_literal_list");
    if (list) {
        collect_nt_children(list, "initializer", out);
    }
}

static char *
format_cstr(const char *fmt, ...)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;
    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    return ncc_buffer_take(buf);
}

static ncc_dict_t *
array_aggregate_types(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_aggregate_types : nullptr;
}

static ncc_dict_t *
array_pointer_typedefs(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_pointer_typedefs : nullptr;
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

static char *
struct_or_union_kind_text(ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *kind = first_descendant_nt(su,
                                                 "_kw_kw_struct_or_union");
    return kind ? node_text(kind) : nullptr;
}

static bool
struct_or_union_is_union(ncc_parse_tree_t *su)
{
    char *kind = struct_or_union_kind_text(su);
    bool  r    = kind && strcmp(kind, "union") == 0;
    ncc_free(kind);
    return r;
}

static char *
aggregate_key_from_specifier(ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
    if (!tag) {
        return nullptr;
    }

    char *kind = struct_or_union_kind_text(su);
    char *name = node_text(tag);
    if (!kind || !name || !*name) {
        ncc_free(kind);
        ncc_free(name);
        return nullptr;
    }

    char *key = format_cstr("%s %s", kind, name);
    ncc_free(kind);
    ncc_free(name);
    return key;
}

static bool
aggregate_specifier_has_members(ncc_parse_tree_t *su)
{
    return su
        && ncc_xform_find_child_nt(su, "member_declaration_list") != nullptr;
}

static array_aggregate_type_info_t *
lookup_aggregate_type(ncc_xform_ctx_t *ctx, const char *key)
{
    ncc_dict_t *types = array_aggregate_types(ctx);
    if (!types || !key) {
        return nullptr;
    }

    bool found = false;
    array_aggregate_type_info_t *info =
        ncc_dict_get(types, (void *)key, &found);
    return found ? info : nullptr;
}

static ncc_parse_tree_t *
resolve_aggregate_specifier(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su)
{
    if (!su) {
        return nullptr;
    }

    if (aggregate_specifier_has_members(su)) {
        return su;
    }

    char *key = aggregate_key_from_specifier(su);
    array_aggregate_type_info_t *info = lookup_aggregate_type(ctx, key);
    ncc_free(key);
    return info ? info->specifier : nullptr;
}

static char *
first_typedef_name_text(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "typedef_name")) {
        return node_text(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = first_typedef_name_text(ncc_tree_child(node, i));
        if (candidate) {
            return candidate;
        }
    }

    return nullptr;
}

static bool
typedef_name_is_pointer(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_dict_t *ptrs = array_pointer_typedefs(ctx);
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

        if (want_pointer && typedef_name_is_pointer(ctx, text)) {
            return copy_cstr(text);
        }

        if (want_aggregate && lookup_aggregate_type(ctx, text)) {
            return copy_cstr(text);
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

static array_aggregate_type_info_t *
aggregate_info_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    if (su) {
        char *key = aggregate_key_from_specifier(su);
        array_aggregate_type_info_t *info = lookup_aggregate_type(ctx, key);
        ncc_free(key);
        if (info) {
            return info;
        }
        if (aggregate_specifier_has_members(su)) {
            return nullptr;
        }
    }

    char *td_name = first_typedef_name_text(specs);
    array_aggregate_type_info_t *info = lookup_aggregate_type(ctx, td_name);
    if (!info && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, false, true);
        info    = lookup_aggregate_type(ctx, td_name);
    }
    ncc_free(td_name);
    return info;
}

static array_aggregate_type_info_t *
aggregate_info_from_type_name(ncc_xform_ctx_t *ctx, const char *type_name)
{
    char *trimmed = trim_copy(type_name);
    array_aggregate_type_info_t *info = lookup_aggregate_type(ctx, trimmed);
    ncc_free(trimmed);
    return info;
}

static bool
decl_specs_use_pointer_typedef(ncc_xform_ctx_t *ctx,
                               ncc_parse_tree_t *specs)
{
    char *td_name = first_typedef_name_text(specs);
    bool  result  = typedef_name_is_pointer(ctx, td_name);
    if (!result && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, true, false);
        result  = typedef_name_is_pointer(ctx, td_name);
    }
    ncc_free(td_name);
    return result;
}

static int
pointer_depth_in_type_node(ncc_parse_tree_t *node)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        return 0;
    }

    int count = 0;
    if (ncc_xform_nt_name_is(node, "pointer")) {
        count++;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += pointer_depth_in_type_node(ncc_tree_child(node, i));
    }

    return count;
}

static int
pointer_depth_in_declarator(ncc_parse_tree_t *node)
{
    return pointer_depth_in_type_node(node);
}

static int
pointer_depth_in_decl_specs(ncc_parse_tree_t *node)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        return 0;
    }

    int count = 0;
    if (ncc_xform_nt_name_is(node, "pointer")) {
        count++;
    }

    if (ncc_xform_nt_name_is(node, "member_declaration_list")) {
        return count;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        count += pointer_depth_in_decl_specs(ncc_tree_child(node, i));
    }
    return count;
}

static int
pointer_depth_for_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    int ptr_depth = pointer_depth_in_decl_specs(specs);
    if (decl_specs_use_pointer_typedef(ctx, specs)) {
        ptr_depth++;
    }
    return ptr_depth;
}

static int
pointer_depth_for_declarator(ncc_xform_ctx_t *ctx,
                             ncc_parse_tree_t *specs,
                             ncc_parse_tree_t *declarator)
{
    return pointer_depth_in_declarator(declarator)
         + pointer_depth_for_specs(ctx, specs);
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
array_declarator_dimensions(ncc_parse_tree_t *declarator,
                            uint64_list_t *dims, ncc_parse_tree_t **bad_array)
{
    char *text = node_text(declarator);
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
        bool ok = parse_positive_integer_literal(bound, &count);
        ncc_free(bound);
        if (!ok) {
            *bad_array = declarator;
            ncc_free(text);
            return false;
        }

        uint64_list_push(dims, count);
        p = close + 1;
    }

    ncc_free(text);
    return dims->len > 0;
}

static bool
has_unbounded_array_declarator(ncc_parse_tree_t *node)
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
        if (has_unbounded_array_declarator(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

static bool
specs_are_unnamed_inline_aggregate(ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    return su && aggregate_specifier_has_members(su)
        && ncc_xform_find_child_nt(su, "tag_name") == nullptr;
}

static char *
implicit_member_field_name(ncc_parse_tree_t *member,
                           ncc_parse_tree_t *member_specs)
{
    char  *text = node_text(member);
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

    ncc_parse_tree_t *su = first_descendant_nt(member_specs,
                                               "struct_or_union_specifier");
    char *td_name = first_typedef_name_text(member_specs);
    if (!su && td_name && strcmp(td_name, name) == 0) {
        ncc_free(td_name);
        ncc_free(name);
        return nullptr;
    }
    ncc_free(td_name);

    ncc_parse_tree_t *tag = su ? ncc_xform_find_child_nt(su, "tag_name")
                               : nullptr;
    if (tag) {
        char *tag_name = node_text(tag);
        if (tag_name && strcmp(tag_name, name) == 0) {
            ncc_free(tag_name);
            ncc_free(name);
            return nullptr;
        }
        ncc_free(tag_name);
    }

    return name;
}

static char *
append_field_path(const char *path, const char *field)
{
    if (path && *path) {
        return format_cstr("%s.%s", path, field);
    }

    return copy_cstr(field);
}

static char *
append_index_path(const char *path, uint64_t ix)
{
    return format_cstr("%s[%llu]", path, (unsigned long long)ix);
}

static char *
offset_expr_for_field_path(const char *type_name, const char *field_path)
{
    return format_cstr(
        "((uint64_t)(__builtin_offsetof(%s,%s)/sizeof(void*)))",
        type_name, field_path);
}

static char *
offset_assert_for_field_path(const char *type_name, const char *field_path)
{
    return format_cstr(
        "_Static_assert((__builtin_offsetof(%s,%s)%%sizeof(void*))==0,"
        "\"array literal pointer field must be pointer-aligned\");",
        type_name, field_path);
}

static void
collect_static_layout_offsets_for_aggregate(ncc_xform_ctx_t *ctx,
                                            ncc_parse_tree_t *site,
                                            const char *elem_type,
                                            ncc_parse_tree_t *aggregate_spec,
                                            const char *base_path,
                                            string_list_t *offsets,
                                            string_list_t *asserts,
                                            int depth);

static ncc_parse_tree_t *
aggregate_spec_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs);

static void
collect_pointer_array_offsets(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                              const char *elem_type, const char *field_path,
                              ncc_parse_tree_t *declarator,
                              string_list_t *offsets, string_list_t *asserts,
                              int dim_index, uint64_list_t *dims)
{
    (void)ctx;
    if ((size_t)dim_index == dims->len) {
        list_push(offsets, offset_expr_for_field_path(elem_type, field_path));
        list_push(asserts, offset_assert_for_field_path(elem_type,
                                                        field_path));
        return;
    }

    for (uint64_t i = 0; i < dims->data[dim_index]; i++) {
        char *indexed = append_index_path(field_path, i);
        collect_pointer_array_offsets(ctx, site, elem_type, indexed,
                                      declarator, offsets, asserts,
                                      dim_index + 1, dims);
        ncc_free(indexed);
    }
}

static void
collect_nested_aggregate_array_offsets(ncc_xform_ctx_t *ctx,
                                       ncc_parse_tree_t *site,
                                       const char *elem_type,
                                       const char *field_path,
                                       ncc_parse_tree_t *nested_spec,
                                       string_list_t *offsets,
                                       string_list_t *asserts,
                                       int dim_index, uint64_list_t *dims,
                                       int depth)
{
    if ((size_t)dim_index == dims->len) {
        collect_static_layout_offsets_for_aggregate(ctx, site, elem_type,
                                                    nested_spec, field_path,
                                                    offsets, asserts, depth);
        return;
    }

    for (uint64_t i = 0; i < dims->data[dim_index]; i++) {
        char *indexed = append_index_path(field_path, i);
        collect_nested_aggregate_array_offsets(ctx, site, elem_type, indexed,
                                               nested_spec, offsets, asserts,
                                               dim_index + 1, dims, depth);
        ncc_free(indexed);
    }
}

static void
collect_member_declarator_offsets(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                                  const char *elem_type,
                                  const char *base_path,
                                  ncc_parse_tree_t *member_specs,
                                  ncc_parse_tree_t *member_declarator,
                                  string_list_t *offsets,
                                  string_list_t *asserts, int depth)
{
    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(member_declarator,
                                                           "declarator");
    if (!declarator) {
        return;
    }

    char *field_name = declarator_name(declarator);
    if (!field_name) {
        return;
    }

    char *field_path = append_field_path(base_path, field_name);
    int   ptr_depth  = pointer_depth_for_declarator(ctx, member_specs,
                                                    declarator);
    bool is_array = first_descendant_nt(declarator, "array_declarator")
                 != nullptr;

    if (ptr_depth > 0) {
        if (is_array) {
            if (has_unbounded_array_declarator(declarator)) {
                array_errorf(site,
                             "static aggregate array literal for '%s' has "
                             "pointer array field '%s' with incomplete bounds; "
                             "use fixed numeric bounds",
                             elem_type, field_path);
            }

            uint64_list_t dims = {0};
            ncc_parse_tree_t *bad = nullptr;
            if (!array_declarator_dimensions(declarator, &dims, &bad)) {
                array_errorf(bad ? bad : site,
                             "static aggregate array literal for '%s' has "
                             "pointer array field '%s' with non-numeric "
                             "bounds; use fixed numeric bounds",
                             elem_type, field_path);
            }

            collect_pointer_array_offsets(ctx, site, elem_type, field_path,
                                          declarator, offsets, asserts, 0,
                                          &dims);
            ncc_free(dims.data);
        }
        else {
            list_push(offsets, offset_expr_for_field_path(elem_type,
                                                          field_path));
            list_push(asserts, offset_assert_for_field_path(elem_type,
                                                            field_path));
        }
    }
    else {
        array_aggregate_type_info_t *nested_info =
            aggregate_info_from_specs(ctx, member_specs);
        ncc_parse_tree_t *nested_spec = nested_info
                                          ? nested_info->specifier
                                          : aggregate_spec_from_specs(ctx,
                                                                     member_specs);
        if (nested_spec) {
            if (is_array) {
                uint64_list_t dims = {0};
                ncc_parse_tree_t *bad = nullptr;
                if (!array_declarator_dimensions(declarator, &dims, &bad)) {
                    array_errorf(bad ? bad : site,
                                 "static aggregate array literal for '%s' has "
                                 "nested aggregate array field '%s' with "
                                 "non-numeric bounds; use fixed numeric bounds",
                                 elem_type, field_path);
                }

                collect_nested_aggregate_array_offsets(
                    ctx, site, elem_type, field_path, nested_spec, offsets,
                    asserts, 0, &dims, depth + 1);
                ncc_free(dims.data);
            }
            else {
                collect_static_layout_offsets_for_aggregate(
                    ctx, site, elem_type, nested_spec, field_path,
                    offsets, asserts, depth + 1);
            }
        }
    }

    ncc_free(field_path);
    ncc_free(field_name);
}

static void
collect_member_declaration_offsets(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *site,
                                   const char *elem_type,
                                   const char *base_path,
                                   ncc_parse_tree_t *member,
                                   string_list_t *offsets,
                                   string_list_t *asserts, int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return;
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");
    if (!member_list) {
        int ptr_depth = pointer_depth_for_specs(ctx, member_specs);

        if (ptr_depth > 0) {
            char *field_name = implicit_member_field_name(member,
                                                          member_specs);
            if (field_name) {
                char *field_path = append_field_path(base_path, field_name);
                list_push(offsets, offset_expr_for_field_path(elem_type,
                                                              field_path));
                list_push(asserts, offset_assert_for_field_path(elem_type,
                                                                field_path));
                ncc_free(field_path);
                ncc_free(field_name);
            }
            return;
        }

        array_aggregate_type_info_t *anonymous_info =
            aggregate_info_from_specs(ctx, member_specs);
        ncc_parse_tree_t *anonymous_spec = anonymous_info
                                             ? anonymous_info->specifier
                                             : aggregate_spec_from_specs(ctx,
                                                                        member_specs);
        if (!anonymous_spec) {
            return;
        }

        char *field_name = implicit_member_field_name(member, member_specs);
        if (!field_name) {
            collect_static_layout_offsets_for_aggregate(
                ctx, site, elem_type, anonymous_spec, base_path,
                offsets, asserts, depth + 1);
            return;
        }

        char *field_path = append_field_path(base_path, field_name);
        collect_static_layout_offsets_for_aggregate(
            ctx, site, elem_type, anonymous_spec, field_path,
            offsets, asserts, depth + 1);
        ncc_free(field_path);
        ncc_free(field_name);
        return;
    }

    ncc_array_t(ncc_parse_tree_ptr_t) declarators =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_nt_children(member_list, "member_declarator", &declarators);

    for (size_t i = 0; i < declarators.len; i++) {
        collect_member_declarator_offsets(ctx, site, elem_type, base_path,
                                          member_specs, declarators.data[i],
                                          offsets, asserts, depth);
    }

    ncc_array_free(declarators);
}

static void
collect_static_layout_offsets_for_aggregate(ncc_xform_ctx_t *ctx,
                                            ncc_parse_tree_t *site,
                                            const char *elem_type,
                                            ncc_parse_tree_t *aggregate_spec,
                                            const char *base_path,
                                            string_list_t *offsets,
                                            string_list_t *asserts,
                                            int depth)
{
    if (depth > 64) {
        array_errorf(site,
                     "static aggregate array literal for '%s' has recursively "
                     "nested aggregate fields",
                     elem_type);
    }

    aggregate_spec = resolve_aggregate_specifier(ctx, aggregate_spec);
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        aggregate_spec, "member_declaration_list");
    if (!members) {
        array_errorf(site,
                     "static aggregate array literal for '%s' uses an "
                     "incomplete or unresolved aggregate type",
                     elem_type);
    }

    bool is_union = struct_or_union_is_union(aggregate_spec);
    int  before   = offsets->count;

    ncc_array_t(ncc_parse_tree_ptr_t) member_decls =
        ncc_array_new(ncc_parse_tree_ptr_t, 8);
    collect_nt_children(members, "member_declaration", &member_decls);

    for (size_t i = 0; i < member_decls.len; i++) {
        collect_member_declaration_offsets(ctx, site, elem_type, base_path,
                                           member_decls.data[i], offsets,
                                           asserts, depth);
    }

    ncc_array_free(member_decls);

    if (is_union && offsets->count > before) {
        array_errorf(site,
                     "static aggregate array literal for union type '%s' has "
                     "pointer-bearing fields; precise active-member static "
                     "layout is not supported yet",
                     elem_type);
    }
}

static char *
join_static_strings(string_list_t *items)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    for (int i = 0; i < items->count; i++) {
        ncc_buffer_puts(buf, items->items[i]);
        if (i + 1 < items->count) {
            ncc_buffer_puts(buf, ",");
        }
    }
    return ncc_buffer_take(buf);
}

static ncc_parse_tree_t *
aggregate_spec_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    if (su) {
        return resolve_aggregate_specifier(ctx, su);
    }

    array_aggregate_type_info_t *info = aggregate_info_from_specs(ctx, specs);
    return info ? info->specifier : nullptr;
}

static void
collect_array_literal_type_info(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu)
{
    ncc_layout_collect_type_info(ctx, tu);
}

static char *
compact_type(const char *type)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    for (const char *p = type; p && *p; p++) {
        if (!isspace((unsigned char)*p)) {
            ncc_buffer_putc(buf, *p);
        }
    }
    return ncc_buffer_take(buf);
}

static bool
is_rstr_element_type(ncc_xform_ctx_t *ctx, const char *type)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    char *compact = compact_type(type);
    char *rstr = compact_type(data->rstr_string_type);
    bool result = strcmp(compact, rstr) == 0
        || strcmp(compact, "ncc_string_ptr_t") == 0
        || strcmp(compact, "ncc_string_t_ptr") == 0
        || strcmp(compact, "n00b_string_ptr_t") == 0
        || strcmp(compact, "n00b_string_t_ptr") == 0;
    ncc_free(compact);
    ncc_free(rstr);
    return result;
}

static bool
is_static_pointer_type(ncc_xform_ctx_t *ctx, const char *type)
{
    if (!type || !*type) {
        return false;
    }

    if (strchr(type, '*')) {
        return true;
    }

    if (is_rstr_element_type(ctx, type)) {
        return true;
    }

    char *trimmed = trim_copy(type);
    bool  result  = ncc_layout_typedef_name_is_pointer(ctx, trimmed);
    ncc_free(trimmed);
    return result;
}

static bool
is_supported_scalar_type(ncc_xform_ctx_t *ctx, const char *type)
{
    if (!type || !*type) {
        return false;
    }

    if (is_static_pointer_type(ctx, type)) {
        return true;
    }

    static const char *known[] = {
        "bool", "char", "signed char", "unsigned char", "short",
        "short int", "signed short", "signed short int", "unsigned short",
        "unsigned short int", "int", "signed", "signed int", "unsigned",
        "unsigned int", "long", "long int", "signed long",
        "signed long int", "unsigned long", "unsigned long int", "long long",
        "long long int", "unsigned long long", "unsigned long long int",
        "float", "double", "long double", "size_t", "int8_t", "int16_t",
        "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "uintptr_t", "intptr_t", nullptr,
    };

    for (int i = 0; known[i]; i++) {
        if (strcmp(type, known[i]) == 0) {
            return true;
        }
    }

    return strncmp(type, "enum ", 5) == 0;
}

static bool
type_policy_skip_word(const char *text, size_t len)
{
    static const char *skip[] = {
        "const", "volatile", "restrict", "_Atomic", "_Nullable",
        "_Nonnull", "_Null_unspecified", "__const__", "__const",
        "__restrict__", "__restrict", "__volatile__", "__volatile",
        "__nullable", "__nonnull", "__null_unspecified", nullptr,
    };

    for (int i = 0; skip[i]; i++) {
        if (strlen(skip[i]) == len && strncmp(text, skip[i], len) == 0) {
            return true;
        }
    }

    return false;
}

static char *
static_policy_type_text(ncc_parse_tree_t *specs)
{
    char         *raw = node_text(specs);
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (const char *p = raw; *p;) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *start = p;
            p++;
            while (isalnum((unsigned char)*p) || *p == '_') {
                p++;
            }

            size_t len = (size_t)(p - start);
            if (type_policy_skip_word(start, len)) {
                continue;
            }
            if (buf->byte_len > 0) {
                ncc_buffer_putc(buf, ' ');
            }
            ncc_buffer_append(buf, start, len);
            continue;
        }

        if (*p != '(' && *p != ')') {
            if (buf->byte_len > 0 && !isspace((unsigned char)buf->data[
                                           buf->byte_len - 1])) {
                ncc_buffer_putc(buf, ' ');
            }
            ncc_buffer_putc(buf, *p);
        }
        p++;
    }

    ncc_free(raw);
    return ncc_buffer_take(buf);
}

static char *
static_policy_field_type_text(ncc_parse_tree_t *specs, const char *field_name)
{
    char *type = static_policy_type_text(specs);
    if (!field_name || !*field_name) {
        return type;
    }

    size_t type_len  = strlen(type);
    size_t field_len = strlen(field_name);
    size_t field_pos = (size_t)-1;

    for (size_t i = 0; type[i];) {
        if (!(isalpha((unsigned char)type[i]) || type[i] == '_')) {
            i++;
            continue;
        }

        size_t start = i;
        i++;
        while (isalnum((unsigned char)type[i]) || type[i] == '_') {
            i++;
        }

        if (i - start == field_len
            && strncmp(type + start, field_name, field_len) == 0) {
            field_pos = start;
        }
    }

    if (field_pos != (size_t)-1 && field_pos > 0) {
        size_t before = field_pos;
        while (before > 0 && isspace((unsigned char)type[before - 1])) {
            before--;
        }

        char *trimmed = ncc_alloc_size(1, before + 1);
        memcpy(trimmed, type, before);
        trimmed[before] = '\0';
        ncc_free(type);
        return trimmed;
    }

    if (type_len <= field_len
        || strcmp(type + type_len - field_len, field_name) != 0) {
        return type;
    }

    size_t before = type_len - field_len;
    if (before == 0
        || (isalnum((unsigned char)type[before - 1])
            || type[before - 1] == '_')) {
        return type;
    }

    while (before > 0 && isspace((unsigned char)type[before - 1])) {
        before--;
    }

    char *trimmed = ncc_alloc_size(1, before + 1);
    memcpy(trimmed, type, before);
    trimmed[before] = '\0';
    ncc_free(type);
    return trimmed;
}

static char *
static_policy_infer_trailing_field_name(char **type_inout)
{
    char  *type = *type_inout;
    size_t len  = strlen(type);
    size_t end  = len;

    while (end > 0 && isspace((unsigned char)type[end - 1])) {
        end--;
    }

    size_t start = end;
    while (start > 0
           && (isalnum((unsigned char)type[start - 1])
               || type[start - 1] == '_')) {
        start--;
    }

    if (start == end
        || (!isalpha((unsigned char)type[start]) && type[start] != '_')) {
        return nullptr;
    }

    int   ident_count = 0;
    char *first       = nullptr;
    for (size_t i = 0; i < start;) {
        while (i < start
               && !(isalpha((unsigned char)type[i]) || type[i] == '_')) {
            i++;
        }
        if (i >= start) {
            break;
        }
        size_t ident_start = i;
        i++;
        while (i < start
               && (isalnum((unsigned char)type[i]) || type[i] == '_')) {
            i++;
        }
        if (!first) {
            size_t first_len = i - ident_start;
            first = ncc_alloc_size(1, first_len + 1);
            memcpy(first, type + ident_start, first_len);
            first[first_len] = '\0';
        }
        ident_count++;
    }

    if (ident_count == 0) {
        ncc_free(first);
        return nullptr;
    }

    bool tag_only = ident_count == 1 && first
                 && (strcmp(first, "struct") == 0
                     || strcmp(first, "union") == 0
                     || strcmp(first, "enum") == 0);
    ncc_free(first);
    if (tag_only) {
        return nullptr;
    }

    char *field = ncc_alloc_size(1, end - start + 1);
    memcpy(field, type + start, end - start);
    field[end - start] = '\0';

    while (start > 0 && isspace((unsigned char)type[start - 1])) {
        start--;
    }

    char *trimmed = ncc_alloc_size(1, start + 1);
    memcpy(trimmed, type, start);
    trimmed[start] = '\0';
    ncc_free(type);
    *type_inout = trimmed;
    return field;
}

static bool
type_name_matches(const char *type, const char *name)
{
    char *type_compact = compact_type(type);
    char *name_compact = compact_type(name);
    bool  result       = strcmp(type_compact, name_compact) == 0;
    ncc_free(type_compact);
    ncc_free(name_compact);
    return result;
}

static const char *
static_layout_denied_type(const char *type)
{
    static const char *denied[] = {
        "n00b_fd_t",        "n00b_buffer_t",    "n00b_dict_untyped_t",
        "n00b_table_t",     "n00b_canvas_t",    "n00b_plane_t",
        "n00b_subproc_t",   "n00b_ffi_module_t", "n00b_rt_option_t",
        "n00b_rt_result_t", "FILE",             "jmp_buf",
        "sigjmp_buf",       "pthread_mutex_t",  "pthread_cond_t",
        "pthread_rwlock_t", nullptr,
    };

    for (int i = 0; denied[i]; i++) {
        if (type_name_matches(type, denied[i])) {
            return denied[i];
        }
    }

    return nullptr;
}

static void validate_static_aggregate_layout(ncc_xform_ctx_t *ctx,
                                             ncc_parse_tree_t *site,
                                             const char *elem_type,
                                             ncc_parse_tree_t *aggregate_spec,
                                             const char *base_path,
                                             int depth);

static void
validate_static_array_bounds(ncc_parse_tree_t *site, const char *elem_type,
                             const char *field_path,
                             ncc_parse_tree_t *declarator)
{
    if (!first_descendant_nt(declarator, "array_declarator")) {
        return;
    }

    if (has_unbounded_array_declarator(declarator)) {
        array_errorf(site,
                     "static aggregate array literal for '%s' has array "
                     "field '%s' with incomplete bounds; use fixed numeric "
                     "bounds",
                     elem_type, field_path);
    }

    uint64_list_t dims = {0};
    ncc_parse_tree_t *bad = nullptr;
    if (!array_declarator_dimensions(declarator, &dims, &bad)) {
        array_errorf(bad ? bad : site,
                     "static aggregate array literal for '%s' has array "
                     "field '%s' with non-numeric bounds; use fixed numeric "
                     "bounds",
                     elem_type, field_path);
    }
    ncc_free(dims.data);
}

static void
reject_static_field_policy(ncc_parse_tree_t *site, const char *elem_type,
                           const char *field_path, const char *field_type)
{
    const char *denied = static_layout_denied_type(field_type);
    if (denied) {
        array_errorf(site,
                     "static aggregate array literal for '%s' has field "
                     "'%s' of transient type '%s', which is not allowed in "
                     "static layout; use a pointer field or initialize this "
                     "object at runtime",
                     elem_type, field_path, denied);
    }

    array_errorf(site,
                 "static aggregate array literal for '%s' has field '%s' of "
                 "type '%s' with no static-layout policy; supported fields "
                 "are scalar numeric/enums, static pointers or r-string "
                 "pointers, fixed-size arrays of those, and nested aggregate "
                 "types with static layout",
                 elem_type, field_path, field_type);
}

static void
validate_static_member_declarator(ncc_xform_ctx_t *ctx,
                                  ncc_parse_tree_t *site,
                                  const char *elem_type,
                                  const char *base_path,
                                  ncc_parse_tree_t *member_specs,
                                  ncc_parse_tree_t *member_declarator,
                                  int depth)
{
    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(member_declarator,
                                                           "declarator");
    if (!declarator) {
        return;
    }

    char *field_name = declarator_name(declarator);
    if (!field_name) {
        return;
    }

    char *field_path = append_field_path(base_path, field_name);
    validate_static_array_bounds(site, elem_type, field_path, declarator);

    int ptr_depth = pointer_depth_for_declarator(ctx, member_specs,
                                                 declarator);
    if (ptr_depth > 0) {
        ncc_free(field_path);
        ncc_free(field_name);
        return;
    }

    array_aggregate_type_info_t *nested_info =
        aggregate_info_from_specs(ctx, member_specs);
    ncc_parse_tree_t *nested_spec = nested_info
                                      ? nested_info->specifier
                                      : aggregate_spec_from_specs(ctx,
                                                                 member_specs);
    if (nested_spec) {
        validate_static_aggregate_layout(ctx, site, elem_type, nested_spec,
                                         field_path, depth + 1);
        ncc_free(field_path);
        ncc_free(field_name);
        return;
    }

    char *field_type = static_policy_field_type_text(member_specs,
                                                     field_name);
    if (!is_supported_scalar_type(ctx, field_type)) {
        reject_static_field_policy(site, elem_type, field_path, field_type);
    }
    ncc_free(field_type);
    ncc_free(field_path);
    ncc_free(field_name);
}

static void
validate_static_member_declaration(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *site,
                                   const char *elem_type,
                                   const char *base_path,
                                   ncc_parse_tree_t *member,
                                   int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return;
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");
    if (member_list) {
        ncc_array_t(ncc_parse_tree_ptr_t) declarators =
            ncc_array_new(ncc_parse_tree_ptr_t, 4);
        collect_nt_children(member_list, "member_declarator", &declarators);

        for (size_t i = 0; i < declarators.len; i++) {
            validate_static_member_declarator(ctx, site, elem_type, base_path,
                                              member_specs,
                                              declarators.data[i], depth);
        }

        ncc_array_free(declarators);
        return;
    }

    int ptr_depth = pointer_depth_for_specs(ctx, member_specs);
    if (ptr_depth > 0) {
        return;
    }

    array_aggregate_type_info_t *anonymous_info =
        aggregate_info_from_specs(ctx, member_specs);
    ncc_parse_tree_t *anonymous_spec = anonymous_info
                                         ? anonymous_info->specifier
                                         : aggregate_spec_from_specs(ctx,
                                                                    member_specs);
    if (anonymous_spec) {
        char *field_name = implicit_member_field_name(member, member_specs);
        if (!field_name) {
            validate_static_aggregate_layout(ctx, site, elem_type,
                                             anonymous_spec, base_path,
                                             depth + 1);
            return;
        }

        char *field_path = append_field_path(base_path, field_name);
        validate_static_aggregate_layout(ctx, site, elem_type, anonymous_spec,
                                         field_path, depth + 1);
        ncc_free(field_path);
        ncc_free(field_name);
        return;
    }

    char *field_name = implicit_member_field_name(member, member_specs);
    char *field_type = static_policy_field_type_text(member_specs,
                                                     field_name);
    if (!field_name) {
        field_name = static_policy_infer_trailing_field_name(&field_type);
    }
    char *field_path = field_name ? append_field_path(base_path, field_name)
                                  : copy_cstr(base_path && *base_path
                                                  ? base_path
                                                  : "<anonymous>");
    if (!is_supported_scalar_type(ctx, field_type)) {
        reject_static_field_policy(site, elem_type, field_path, field_type);
    }
    ncc_free(field_type);
    ncc_free(field_path);
    ncc_free(field_name);
}

static void
validate_static_aggregate_layout(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                                 const char *elem_type,
                                 ncc_parse_tree_t *aggregate_spec,
                                 const char *base_path,
                                 int depth)
{
    if (depth > 64) {
        array_errorf(site,
                     "static aggregate array literal for '%s' has recursively "
                     "nested aggregate fields",
                     elem_type);
    }

    aggregate_spec = resolve_aggregate_specifier(ctx, aggregate_spec);
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        aggregate_spec, "member_declaration_list");
    if (!members) {
        array_errorf(site,
                     "static aggregate array literal for '%s' uses an "
                     "incomplete or unresolved aggregate type",
                     elem_type);
    }

    ncc_array_t(ncc_parse_tree_ptr_t) member_decls =
        ncc_array_new(ncc_parse_tree_ptr_t, 8);
    collect_nt_children(members, "member_declaration", &member_decls);

    for (size_t i = 0; i < member_decls.len; i++) {
        validate_static_member_declaration(ctx, site, elem_type, base_path,
                                           member_decls.data[i], depth);
    }

    ncc_array_free(member_decls);
}

static void
array_errorf(ncc_parse_tree_t *node, const char *fmt, ...)
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

static void
array_error(ncc_parse_tree_t *node, const char *msg, const char *detail)
{
    if (detail) {
        array_errorf(node, "%s: %s", msg, detail);
    }
    else {
        array_errorf(node, "%s", msg);
    }
}

static void
insert_generated_decl(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                      const char *src)
{
    ncc_parse_tree_t *wrapper = ncc_xform_find_ancestor(decl, "block_item");
    const char       *nt_name = "block_item_list";
    const char       *item_nt = "block_item";

    if (!wrapper) {
        wrapper = ncc_xform_find_ancestor(decl, "external_declaration");
        nt_name = "translation_unit";
        item_nt = "external_declaration";
    }

    if (!wrapper) {
        array_error(decl, "array literal transform could not find insertion "
                          "context", nullptr);
    }

    ncc_parse_tree_t *tree = ncc_xform_parse_source(ctx->grammar, nt_name, src,
                                                    "xform_array_literal");
    if (!tree) {
        fprintf(stderr, "ncc: error: failed to parse generated declaration:\n%s\n",
                src);
        exit(1);
    }

    ncc_array_t(ncc_parse_tree_ptr_t) generated =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_nt_children(tree, item_nt, &generated);
    if (generated.len == 0 && ncc_xform_nt_name_is(tree, item_nt)) {
        parse_tree_array_push(&generated, tree);
    }

    ncc_nt_node_t    pn        = ncc_tree_node_value(wrapper);
    ncc_parse_tree_t *container = pn.parent ? pn.parent : ctx->root;

    for (size_t g = 0; g < generated.len; g++) {
        ncc_parse_tree_t *item = generated.data[g];
        ncc_token_info_t *last = ncc_xform_find_last_leaf_token(item);
        if (last) {
            ncc_trivia_t *nl = ncc_alloc(ncc_trivia_t);
            nl->text = ncc_string_from_cstr("\n");
            nl->next = last->trailing_trivia;
            last->trailing_trivia = nl;
        }

        size_t insert_pos = 0;
        size_t nc = ncc_tree_num_children(container);
        for (size_t i = 0; i < nc; i++) {
            if (ncc_tree_child(container, i) == wrapper) {
                insert_pos = i;
                break;
            }
        }

        ncc_xform_insert_child(container, insert_pos, item);
    }

    ncc_array_free(generated);
}

static char *lower_array_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                                 ncc_parse_tree_t *literal,
                                 array_type_info_t *type,
                                 string_list_t *decls, bool materialize);

static char *
build_array_initializer(const char *data_name, int count)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "{.data=%s,.len=%d,.cap=%d,.lock=nullptr,"
                      ".allocator=nullptr,.scan_kind=0,.scan_cb=nullptr,"
                      ".scan_user=nullptr}",
                      data_name ? data_name : "nullptr", count, count);
    return ncc_buffer_take(buf);
}

static char *
join_initializer_exprs(string_list_t *exprs)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (int i = 0; i < exprs->count; i++) {
        if (i > 0) {
            ncc_buffer_puts(buf, ",");
        }
        ncc_buffer_puts(buf, exprs->items[i]);
    }

    return ncc_buffer_take(buf);
}

static bool
is_rstr_call_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "postfix_expression")
        || ncc_tree_num_children(node) < 3
        || !ncc_xform_leaf_text_eq(ncc_tree_child(node, 1), "(")) {
        return false;
    }

    const char *name = ncc_xform_get_first_leaf_text(ncc_tree_child(node, 0));
    return name && strcmp(name, "__ncc_rstr") == 0;
}

static void
collect_rstr_calls(ncc_parse_tree_t *node,
                   ncc_array_t(ncc_parse_tree_ptr_t) *calls)
{
    if (!node) {
        return;
    }

    if (is_rstr_call_node(node)) {
        parse_tree_array_push(calls, node);
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_rstr_calls(ncc_tree_child(node, i), calls);
    }
}

static char *
replace_first_substr(const char *text, const char *needle,
                     const char *replacement)
{
    char *hit = strstr(text, needle);
    if (!hit) {
        return nullptr;
    }

    size_t prefix_len = (size_t)(hit - text);
    size_t needle_len = strlen(needle);
    size_t repl_len   = strlen(replacement);
    size_t suffix_len = strlen(hit + needle_len);

    char *result = ncc_alloc_size(1, prefix_len + repl_len + suffix_len + 1);
    memcpy(result, text, prefix_len);
    memcpy(result + prefix_len, replacement, repl_len);
    memcpy(result + prefix_len + repl_len, hit + needle_len, suffix_len);
    result[prefix_len + repl_len + suffix_len] = '\0';
    return result;
}

static bool
lower_rstrings_in_initializer_expr(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *init,
                                   string_list_t *decls,
                                   char **expr_inout)
{
    ncc_array_t(ncc_parse_tree_ptr_t) calls =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_rstr_calls(init, &calls);

    if (calls.len == 0) {
        ncc_array_free(calls);
        return false;
    }

    for (size_t i = 0; i < calls.len; i++) {
        char *call_text = node_text(calls.data[i]);
        ncc_rstr_static_ref_t rstr = ncc_rstr_build_static_ref(ctx,
                                                               calls.data[i]);
        if (!rstr.decl || !rstr.expr) {
            ncc_free(call_text);
            ncc_array_free(calls);
            array_error(init, "could not lower r-string array element",
                        nullptr);
        }

        char *rewritten = replace_first_substr(*expr_inout, call_text,
                                               rstr.expr);
        if (!rewritten) {
            ncc_free(call_text);
            ncc_free(rstr.decl);
            ncc_free(rstr.expr);
            ncc_array_free(calls);
            array_error(init,
                        "could not splice generated r-string reference into "
                        "array initializer",
                        nullptr);
        }

        list_push(decls, rstr.decl);
        ncc_free(*expr_inout);
        *expr_inout = rewritten;
        ncc_free(call_text);
        ncc_free(rstr.expr);
    }

    ncc_array_free(calls);
    return true;
}

static char *
data_pointer_type_name(const char *elem_type)
{
    size_t len = strlen(elem_type);
    char  *result = ncc_alloc_size(1, len + 3);

    memcpy(result, elem_type, len);
    memcpy(result + len, " *", 3);
    return result;
}

static void
array_scan_plan_init(array_scan_plan_t *plan, ncc_xform_ctx_t *ctx,
                     array_type_info_t *type, array_type_info_t *elem_array,
                     const char *data_name, int count, bool emit_shape_decl,
                     ncc_parse_tree_t *site)
{
    bool elem_is_pointer = is_static_pointer_type(ctx, type->elem_type);
    bool elem_is_nested  = elem_array != nullptr;
    array_aggregate_type_info_t *aggregate =
        elem_is_nested || elem_is_pointer
            ? nullptr
            : aggregate_info_from_type_name(ctx, type->elem_type);

    snprintf(plan->ptr_words, sizeof(plan->ptr_words), "%d",
             elem_is_pointer && !elem_is_nested ? count : 0);
    snprintf(plan->shape_name, sizeof(plan->shape_name),
             "__ncc_arraylit_%s_shape", data_name);
    snprintf(plan->stride_words, sizeof(plan->stride_words),
             "(sizeof(%s)/sizeof(void*))", type->elem_type);

    plan->scan_kind  = "1";
    plan->scan_cb    = "nullptr";
    plan->scan_user  = "nullptr";
    plan->no_scan    = "true";
    plan->shape_decl = copy_cstr("");

    if (elem_is_nested) {
        plan->scan_kind = "4";
        plan->scan_cb   = "n00b_gc_scan_cb_struct_field";
        plan->no_scan   = "false";

        if (emit_shape_decl) {
            ncc_buffer_t *shape = ncc_buffer_empty();
            ncc_buffer_printf(shape,
                              "static n00b_gc_struct_array_t %s={.stride=%s,"
                              ".offset=0,.count=%d};",
                              plan->shape_name, plan->stride_words, count);
            ncc_free(plan->shape_decl);
            plan->shape_decl = ncc_buffer_take(shape);

            ncc_buffer_t *shape_ref = ncc_buffer_empty();
            ncc_buffer_printf(shape_ref, "&%s", plan->shape_name);
            plan->owned_scan_user = ncc_buffer_take(shape_ref);
            plan->scan_user       = plan->owned_scan_user;
        }
        else {
            plan->scan_user = "__unused_shape";
        }
    }
    else if (elem_is_pointer) {
        plan->scan_kind = "2";
        plan->no_scan   = "false";
    }
    else if (aggregate) {
        string_list_t offsets = {0};
        string_list_t asserts = {0};
        collect_static_layout_offsets_for_aggregate(
            ctx, site, type->elem_type, aggregate->specifier, "", &offsets,
            &asserts, 0);

        snprintf(plan->ptr_words, sizeof(plan->ptr_words), "%d",
                 offsets.count * count);

        if (offsets.count > 0) {
            plan->scan_kind = "4";
            plan->scan_cb   = "n00b_gc_scan_cb_struct_layout";
            plan->no_scan   = "false";

            if (emit_shape_decl) {
                char *offset_values = join_static_strings(&offsets);

                ncc_buffer_t *shape = ncc_buffer_empty();
                for (int i = 0; i < asserts.count; i++) {
                    ncc_buffer_puts(shape, asserts.items[i]);
                }
                ncc_buffer_printf(
                    shape,
                    "_Static_assert((sizeof(%s)%%sizeof(void*))==0,"
                    "\"array literal aggregate element must be word-sized\");"
                    "static const uint64_t %s_offsets[]={%s};"
                    "static n00b_gc_struct_layout_t %s={.stride=%s,"
                    ".count=%d,.offset_count=%d,.offsets=%s_offsets};",
                    type->elem_type, plan->shape_name, offset_values,
                    plan->shape_name, plan->stride_words, count,
                    offsets.count, plan->shape_name);
                ncc_free(offset_values);
                ncc_free(plan->shape_decl);
                plan->shape_decl = ncc_buffer_take(shape);

                ncc_buffer_t *shape_ref = ncc_buffer_empty();
                ncc_buffer_printf(shape_ref, "&%s", plan->shape_name);
                plan->owned_scan_user = ncc_buffer_take(shape_ref);
                plan->scan_user       = plan->owned_scan_user;
            }
            else {
                plan->scan_user = "__unused_shape";
            }
        }

        list_free(&offsets);
        list_free(&asserts);
    }
}

static void
array_scan_plan_free(array_scan_plan_t *plan)
{
    ncc_free(plan->owned_scan_user);
    ncc_free(plan->shape_decl);
}

static char *
build_array_data_decl(ncc_xform_ctx_t *ctx, array_type_info_t *type,
                      array_type_info_t *elem_array, const char *data_name,
                      const char *wrapper_name, string_list_t *exprs,
                      ncc_parse_tree_t *site)
{
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d", exprs->count);

    char *items = join_initializer_exprs(exprs);

    char *ptr_type = data_pointer_type_name(type->elem_type);
    char *typehash_str = ncc_static_object_typehash_expr(ptr_type);
    ncc_free(ptr_type);

    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, type, elem_array, data_name,
                         exprs->count, true, site);

    ncc_static_object_names_t names;
    ncc_static_object_names_for_array(&names, data_name);

    ncc_static_object_slots_t stobj;
    ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2",
                                 scan_plan.scan_kind, scan_plan.scan_cb,
                                 scan_plan.scan_user,
                                 "N00B_STATIC_IDENTITY_NCC_ARRAY_DATA",
                                 "ncc-array-data", site, count_str);

    const char *all_args[] = {
        type->elem_type, data_name, count_str, items, stobj.typehash,
        scan_plan.ptr_words, stobj.scan_kind, stobj.scan_cb, stobj.scan_user,
        wrapper_name, scan_plan.shape_name, scan_plan.stride_words, "0",
        scan_plan.shape_decl, scan_plan.no_scan, stobj.desc_name,
        stobj.entry_name, stobj.object_id, stobj.flags, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
    };

    char *result = ncc_static_object_expand_template(
        "array literal data", get_array_literal_data_template(ctx),
        all_args, 22);

    ncc_static_object_slots_cleanup(&stobj);
    ncc_free(items);
    ncc_free(typehash_str);
    array_scan_plan_free(&scan_plan);
    return result;
}

static char *
build_array_data_expr(ncc_xform_ctx_t *ctx, array_type_info_t *type,
                      array_type_info_t *elem_array, const char *data_name,
                      const char *wrapper_name, int count,
                      ncc_parse_tree_t *site)
{
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d", count);

    char *ptr_type = data_pointer_type_name(type->elem_type);
    char *typehash_str = ncc_static_object_typehash_expr(ptr_type);
    ncc_free(ptr_type);

    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, type, elem_array, data_name,
                         count, false, site);

    ncc_static_object_names_t names;
    ncc_static_object_names_for_array(&names, data_name);

    ncc_static_object_slots_t stobj;
    ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2",
                                 scan_plan.scan_kind, scan_plan.scan_cb,
                                 scan_plan.scan_user,
                                 "N00B_STATIC_IDENTITY_NCC_ARRAY_DATA",
                                 "ncc-array-data", site, count_str);

    const char *all_args[] = {
        type->elem_type, data_name, count_str, "", stobj.typehash,
        scan_plan.ptr_words, stobj.scan_kind, stobj.scan_cb, stobj.scan_user,
        wrapper_name, "__unused_shape", "0", "0", "",
        scan_plan.no_scan, stobj.desc_name, stobj.entry_name,
        stobj.object_id, stobj.flags, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
    };

    char *result = ncc_static_object_expand_template(
        "array literal data expression", get_array_literal_data_expr(ctx),
        all_args, 22);

    ncc_static_object_slots_cleanup(&stobj);
    ncc_free(typehash_str);
    array_scan_plan_free(&scan_plan);
    return result;
}

static char *
lower_array_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                    ncc_parse_tree_t *literal, array_type_info_t *type,
                    string_list_t *decls, bool materialize)
{
    ncc_array_t(ncc_parse_tree_ptr_t) elems =
        ncc_array_new(ncc_parse_tree_ptr_t, 8);
    collect_array_elements(literal, &elems);

    string_list_t exprs = {0};
    array_type_info_t *elem_array = lookup_array_type(ctx, type->elem_type);
    array_aggregate_type_info_t *elem_aggregate =
        elem_array ? nullptr : aggregate_info_from_type_name(ctx,
                                                             type->elem_type);

    if (!elem_array && !elem_aggregate
        && !is_supported_scalar_type(ctx, type->elem_type)) {
        array_errorf(literal,
                     "array literal element type '%s' is not supported for "
                     "static initialization yet; allowed element types are "
                     "scalar numeric/enums, static pointers, r-string "
                     "pointers, aggregate types with static layout, and "
                     "nested ncc/n00b arrays",
                     type->elem_type);
    }

    if (elem_aggregate) {
        validate_static_aggregate_layout(ctx, literal, type->elem_type,
                                         elem_aggregate->specifier, "", 0);
    }

    for (size_t i = 0; i < elems.len; i++) {
        ncc_parse_tree_t *init = elems.data[i];
        ncc_parse_tree_t *nested = initializer_array_literal(init);

        if (elem_array) {
            if (!nested) {
                array_errorf(init,
                             "nested array element for '%s' must be an array "
                             "literal like '[...]'",
                             type->elem_type);
            }
            char *expr = lower_array_literal(ctx, decl, nested, elem_array,
                                             decls, false);
            list_push(&exprs, expr);
            continue;
        }

        if (nested) {
            array_errorf(init,
                         "array element type '%s' is scalar, so nested array "
                         "literal '[...]' is not allowed here",
                         type->elem_type);
        }

        char *expr = node_text(init);
        if (strstr(expr, "__ncc_rstr")) {
            if (!elem_aggregate && !is_rstr_element_type(ctx,
                                                         type->elem_type)) {
                array_errorf(init,
                             "r-string array element requires element type "
                             "'%s' or a configured r-string pointer type; "
                             "found '%s'",
                             ncc_xform_get_data(ctx)->rstr_string_type,
                             type->elem_type);
            }
            (void)lower_rstrings_in_initializer_expr(ctx, init, decls,
                                                     &expr);
            list_push(&exprs, expr);
            continue;
        }
        list_push(&exprs, expr);
    }

    char *data_name = nullptr;
    char *data_expr = nullptr;
    if (exprs.count > 0) {
        int id = ctx->unique_id++;
        ncc_buffer_t *name = ncc_buffer_empty();
        ncc_buffer_printf(name, "__ncc_arraylit_%d_data", id);
        data_name = ncc_buffer_take(name);

        char wrapper_name[64];
        snprintf(wrapper_name, sizeof(wrapper_name), "%s_storage",
                 data_name);

        char *data_decl = build_array_data_decl(ctx, type, elem_array,
                                                data_name, wrapper_name,
                                                &exprs, literal);
        list_push(decls, data_decl);
        data_expr = build_array_data_expr(ctx, type, elem_array, data_name,
                                          wrapper_name, exprs.count, literal);
    }

    char *init = build_array_initializer(data_expr ? data_expr : data_name,
                                         exprs.count);
    ncc_free(data_expr);
    ncc_free(data_name);
    list_free(&exprs);
    ncc_array_free(elems);

    if (!materialize) {
        return init;
    }

    int id = ctx->unique_id++;
    ncc_buffer_t *obj_name_buf = ncc_buffer_empty();
    ncc_buffer_printf(obj_name_buf, "__ncc_arraylit_%d_obj", id);
    char *obj_name = ncc_buffer_take(obj_name_buf);

    ncc_buffer_t *obj_decl = ncc_buffer_empty();
    ncc_buffer_printf(obj_decl, "static %s %s = %s;",
                      type->object_type, obj_name, init);
    list_push(decls, ncc_buffer_take(obj_decl));

    ncc_free(init);
    return obj_name;
}

static bool
decl_is_typedef(ncc_parse_tree_t *decl_specs)
{
    return contains_leaf_text(decl_specs, "typedef");
}

static bool
decl_is_const(ncc_parse_tree_t *decl_specs)
{
    return contains_leaf_text(decl_specs, "const");
}

static void
record_typedef_aliases(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                       ncc_parse_tree_t *decl_specs,
                       array_type_info_t *base_info)
{
    if (!base_info || !decl_is_typedef(decl_specs)) {
        return;
    }

    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!list) {
        return;
    }

    ncc_array_t(ncc_parse_tree_ptr_t) init_decls =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_nt_children(list, "init_declarator", &init_decls);

    for (size_t i = 0; i < init_decls.len; i++) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            init_decls.data[i], "declarator");
        char *alias = declarator_name(declarator);
        if (alias) {
            record_array_type(ctx, alias, alias, base_info->elem_type);
            ncc_free(alias);
        }
    }

    ncc_array_free(init_decls);
}

static ncc_parse_tree_t *
xform_array_decl(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                 ncc_xform_control_t *control)
{
    (void)control;

    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        decl, "declaration_specifiers");
    if (!decl_specs) {
        return nullptr;
    }

    array_type_info_t *type = array_type_from_decl_specs(ctx, decl_specs);
    record_typedef_aliases(ctx, decl, decl_specs, type);

    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!list) {
        return nullptr;
    }

    ncc_array_t(ncc_parse_tree_ptr_t) init_decls =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_nt_children(list, "init_declarator", &init_decls);

    for (size_t i = 0; i < init_decls.len; i++) {
        ncc_parse_tree_t *init = ncc_xform_find_child_nt(init_decls.data[i],
                                                         "initializer");
        ncc_parse_tree_t *literal = initializer_array_literal(init);
        if (!literal) {
            continue;
        }

        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            init_decls.data[i], "declarator");
        char *name = declarator_name(declarator);

        if (!type) {
            char *target = node_text(decl_specs);
            array_errorf(literal,
                         "array literal initializer for '%s' requires an "
                         "ncc_array_t(T) or n00b_array_t(T) object target; "
                         "found target type '%s'",
                         name ? name : "<unknown>", target);
        }

        if (count_leaf_text(declarator, "*") > 0
            || count_leaf_text(declarator, "^") > 0) {
            array_errorf(literal,
                         "array literal initializer for '%s' must target an "
                         "array object, not a pointer; declare an "
                         "ncc_array_t(T) or n00b_array_t(T) value",
                         name ? name : "<unknown>");
        }

        bool local = ncc_xform_find_ancestor(decl, "block_item") != nullptr;
        if (local && !decl_is_const(decl_specs)) {
            array_errorf(literal,
                         "non-const local array literal for '%s' is not "
                         "supported yet; add 'const' or move the declaration "
                         "to file scope",
                         name ? name : "<unknown>");
        }

        string_list_t decls = {0};
        char *replacement_src = lower_array_literal(ctx, decl, literal, type,
                                                    &decls, false);

        for (int d = 0; d < decls.count; d++) {
            insert_generated_decl(ctx, decl, decls.items[d]);
        }

        ncc_parse_tree_t *replacement = ncc_xform_parse_source(
            ctx->grammar, "initializer", replacement_src,
            "xform_array_literal");
        if (!replacement) {
            fprintf(stderr, "ncc: error: failed to parse generated array "
                            "initializer:\n%s\n", replacement_src);
            exit(1);
        }

        replace_initializer(init_decls.data[i], replacement);
        ncc_free(replacement_src);
        ncc_free(name);
        list_free(&decls);
    }

    ncc_array_free(init_decls);
    return nullptr;
}

static ncc_parse_tree_t *
xform_array_translation_unit(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
                             ncc_xform_control_t *control)
{
    (void)control;
    collect_array_literal_type_info(ctx, tu);
    return nullptr;
}

void
ncc_register_array_literal_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "translation_unit",
                           xform_array_translation_unit,
                           "array_literal_type_info");
    ncc_xform_register_pre(reg, "declaration", xform_array_decl,
                           "array_literal");
}
