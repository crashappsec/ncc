// xform_array_literal.c -- Transform: ncc/n00b array literal initializers.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/dict.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_rstr.h"
#include "xform/xform_static_object.h"

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
is_supported_scalar_type(ncc_xform_ctx_t *ctx, const char *type)
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
                     const char *data_name, int count, bool emit_shape_decl)
{
    bool elem_is_pointer = strchr(type->elem_type, '*') != nullptr
                         || is_rstr_element_type(ctx, type->elem_type);
    bool elem_is_nested  = elem_array != nullptr;

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
                      const char *wrapper_name, string_list_t *exprs)
{
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d", exprs->count);

    char *items = join_initializer_exprs(exprs);

    char *ptr_type = data_pointer_type_name(type->elem_type);
    char *typehash_str = ncc_static_object_typehash_expr(ptr_type);
    ncc_free(ptr_type);

    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, type, elem_array, data_name,
                         exprs->count, true);

    ncc_static_object_names_t names;
    ncc_static_object_names_for_array(&names, data_name);

    ncc_static_object_slots_t stobj;
    ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2",
                                 scan_plan.scan_kind, scan_plan.scan_cb,
                                 scan_plan.scan_user);

    const char *all_args[] = {
        type->elem_type, data_name, count_str, items, stobj.typehash,
        scan_plan.ptr_words, stobj.scan_kind, stobj.scan_cb, stobj.scan_user,
        wrapper_name, scan_plan.shape_name, scan_plan.stride_words, "0",
        scan_plan.shape_decl, scan_plan.no_scan, stobj.desc_name,
        stobj.entry_name, stobj.object_id, stobj.flags, stobj.entry_attr,
    };

    char *result = ncc_static_object_expand_template(
        "array literal data", get_array_literal_data_template(ctx),
        all_args, 20);

    ncc_free(items);
    ncc_free(typehash_str);
    array_scan_plan_free(&scan_plan);
    return result;
}

static char *
build_array_data_expr(ncc_xform_ctx_t *ctx, array_type_info_t *type,
                      array_type_info_t *elem_array, const char *data_name,
                      const char *wrapper_name, int count)
{
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d", count);

    char *ptr_type = data_pointer_type_name(type->elem_type);
    char *typehash_str = ncc_static_object_typehash_expr(ptr_type);
    ncc_free(ptr_type);

    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, type, elem_array, data_name,
                         count, false);

    ncc_static_object_names_t names;
    ncc_static_object_names_for_array(&names, data_name);

    ncc_static_object_slots_t stobj;
    ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2",
                                 scan_plan.scan_kind, scan_plan.scan_cb,
                                 scan_plan.scan_user);

    const char *all_args[] = {
        type->elem_type, data_name, count_str, "", stobj.typehash,
        scan_plan.ptr_words, stobj.scan_kind, stobj.scan_cb, stobj.scan_user,
        wrapper_name, "__unused_shape", "0", "0", "",
        scan_plan.no_scan, stobj.desc_name, stobj.entry_name,
        stobj.object_id, stobj.flags, stobj.entry_attr,
    };

    char *result = ncc_static_object_expand_template(
        "array literal data expression", get_array_literal_data_expr(ctx),
        all_args, 20);

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

    if (!elem_array && !is_supported_scalar_type(ctx, type->elem_type)) {
        array_errorf(literal,
                     "array literal element type '%s' is not supported for "
                     "static initialization yet; allowed element types are "
                     "scalar numeric/enums, static pointers, r-string "
                     "pointers, and nested ncc/n00b arrays",
                     type->elem_type);
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
            if (!is_rstr_element_type(ctx, type->elem_type)) {
                array_errorf(init,
                             "r-string array element requires element type "
                             "'%s' or a configured r-string pointer type; "
                             "found '%s'",
                             ncc_xform_get_data(ctx)->rstr_string_type,
                             type->elem_type);
            }
            ncc_free(expr);
            ncc_rstr_static_ref_t rstr = ncc_rstr_build_static_ref(ctx, init);
            if (!rstr.decl || !rstr.expr) {
                array_error(init, "could not lower r-string array element",
                            nullptr);
            }
            list_push(decls, rstr.decl);
            list_push(&exprs, rstr.expr);
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
                                                &exprs);
        list_push(decls, data_decl);
        data_expr = build_array_data_expr(ctx, type, elem_array, data_name,
                                          wrapper_name, exprs.count);
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

void
ncc_register_array_literal_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "declaration", xform_array_decl,
                           "array_literal");
}
