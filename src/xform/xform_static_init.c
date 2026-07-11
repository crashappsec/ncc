// xform_static_init.c -- WP-005 static-initializer prologue lowering.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/type_infer.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ncc_parse_tree_t **items;
    size_t             len;
    size_t             cap;
} node_list_t;

typedef struct {
    size_t slot;
    char  *value_type;
    char  *value_expr;
} static_addr_patch_t;

typedef struct {
    static_addr_patch_t *items;
    size_t               len;
    size_t               cap;
} static_addr_patch_list_t;

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
copy_span(const char *start, size_t len)
{
    char *out = ncc_alloc_size(1, len + 1);
    if (len > 0) {
        memcpy(out, start, len);
    }
    out[len] = '\0';
    return out;
}

static void
static_addr_patch_list_push(static_addr_patch_list_t *list,
                            size_t slot, char *value_type, char *value_expr)
{
    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2 : 4;
        static_addr_patch_t *next =
            ncc_alloc_array(static_addr_patch_t, next_cap);
        if (list->items) {
            memcpy(next, list->items, list->len * sizeof(*next));
            ncc_free(list->items);
        }
        list->items = next;
        list->cap   = next_cap;
    }

    list->items[list->len++] = (static_addr_patch_t){
        .slot       = slot,
        .value_type = value_type,
        .value_expr = value_expr,
    };
}

static void
static_addr_patch_list_free(static_addr_patch_list_t *list)
{
    for (size_t i = 0; i < list->len; i++) {
        ncc_free(list->items[i].value_type);
        ncc_free(list->items[i].value_expr);
    }
    ncc_free(list->items);
}

static void
collect_static_addr_patch_markers(const char *init_expr,
                                  static_addr_patch_list_t *patches)
{
    static const char marker[] = "__ncc_static_addr_patch|";
    const size_t marker_len = sizeof(marker) - 1;

    const char *p = init_expr;
    while (p && (p = strstr(p, marker)) != nullptr) {
        p += marker_len;

        char *slot_end = nullptr;
        unsigned long long slot = strtoull(p, &slot_end, 10);
        if (slot_end == p || !slot_end || *slot_end != '|') {
            continue;
        }

        const char *type_start = slot_end + 1;
        const char *type_end   = strchr(type_start, '|');
        if (!type_end) {
            break;
        }

        const char *expr_start = type_end + 1;
        const char *expr_end   = strchr(expr_start, '"');
        if (!expr_end) {
            break;
        }

        static_addr_patch_list_push(
            patches, (size_t)slot,
            copy_span(type_start, (size_t)(type_end - type_start)),
            copy_span(expr_start, (size_t)(expr_end - expr_start)));
        p = expr_end + 1;
    }
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

    char *out = ncc_alloc_size(1, len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
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
node_list_push(node_list_t *list, ncc_parse_tree_t *node)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 4;
        list->items = ncc_realloc(list->items,
                                  new_cap * sizeof(*list->items));
        list->cap = new_cap;
    }

    list->items[list->len++] = node;
}

static void
node_list_free(node_list_t *list)
{
    if (!list) {
        return;
    }

    ncc_free(list->items);
    *list = (node_list_t){0};
}

static void
collect_nt_children(ncc_parse_tree_t *node, const char *name,
                    node_list_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (ncc_xform_nt_name_is(child, name)) {
            node_list_push(out, child);
            continue;
        }
        if (is_group_node(child)) {
            collect_nt_children(child, name, out);
        }
    }
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
c_identifier_is_valid(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }

    unsigned char first = (unsigned char)name[0];
    if (!(isalpha(first) || first == '_')) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)name + 1; *p; p++) {
        if (!(isalnum(*p) || *p == '_')) {
            return false;
        }
    }

    return true;
}

static bool
text_is_empty_initializer(const char *s)
{
    if (!s) {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s++ != '{') {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s++ != '}') {
        return false;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    return *s == '\0';
}

static bool
text_looks_like_array_value_initializer(const char *s)
{
    if (!s) {
        return false;
    }

    size_t len = strlen(s);
    char *compact = ncc_alloc_size(1, len + 1);
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)s[i])) {
            compact[out++] = s[i];
        }
    }
    compact[out] = '\0';

    bool result = strstr(compact, ".data=") != nullptr
        && strstr(compact, ".len=") != nullptr
        && strstr(compact, ".cap=") != nullptr
        && strstr(compact, ".scan_kind=") != nullptr;
    ncc_free(compact);
    return result;
}

static bool
text_looks_like_dict_value_initializer(const char *s)
{
    if (!s) {
        return false;
    }

    size_t len = strlen(s);
    char *compact = ncc_alloc_size(1, len + 1);
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (!isspace((unsigned char)s[i])) {
            compact[out++] = s[i];
        }
    }
    compact[out] = '\0';

    bool result = strstr(compact, ".store=") != nullptr
        && strstr(compact, ".length=") != nullptr
        && strstr(compact, ".skip_obj_hash=") != nullptr;
    ncc_free(compact);
    return result;
}

static bool
text_uses_static_payload_allocator(const char *s)
{
    return s && strstr(s, "n00b_crt_alloc_static_payload") != nullptr;
}

static ncc_sym_entry_t *
lookup_static_init_entry(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->symtab || !name) {
        return nullptr;
    }

    ncc_string_t key = {
        .data = (char *)name,
        .u8_bytes = strlen(name),
    };
    ncc_sym_entry_t *entry =
        ncc_symtab_lookup(data->symtab, ncc_string_empty(), key);

    return entry && entry->kind == NCC_SYM_VARIABLE && entry->is_static_init
               ? entry
               : nullptr;
}

static bool
symbol_type_is_pointer(ncc_sym_entry_t *entry)
{
    char *type = ncc_type_of_symbol(entry);
    if (!type) {
        return false;
    }

    size_t len = strlen(type);
    while (len > 0 && isspace((unsigned char)type[len - 1])) {
        len--;
    }

    bool result = len > 0 && type[len - 1] == '*';
    ncc_free(type);
    return result;
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
add_trailing_newline(ncc_parse_tree_t *item)
{
    ncc_token_info_t *last = ncc_xform_find_last_leaf_token(item);
    if (!last) {
        return;
    }

    ncc_trivia_t *nl = ncc_alloc(ncc_trivia_t);
    nl->text = ncc_string_from_cstr("\n");
    nl->next = last->trailing_trivia;
    last->trailing_trivia = nl;
}

static void
insert_external_decls_after(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                            const char *src)
{
    ncc_parse_tree_t *wrapper =
        ncc_xform_find_ancestor(decl, "external_declaration");
    if (!wrapper) {
        fprintf(stderr,
                "ncc: error: static-init transform could not find file-scope "
                "insertion context\n");
        exit(1);
    }

    ncc_parse_tree_t *tree = ncc_xform_parse_source(
        ctx->grammar, "translation_unit", src, "xform_static_init");
    if (!tree) {
        fprintf(stderr,
                "ncc: error: failed to parse generated static-init helper:\n%s\n",
                src);
        exit(1);
    }

    node_list_t generated = {0};
    collect_nt_children(tree, "external_declaration", &generated);
    if (generated.len == 0
        && ncc_xform_nt_name_is(tree, "external_declaration")) {
        node_list_push(&generated, tree);
    }

    ncc_nt_node_t    pn        = ncc_tree_node_value(wrapper);
    ncc_parse_tree_t *container = pn.parent ? pn.parent : ctx->root;
    size_t            insert_pos = ncc_tree_num_children(container);
    size_t            nc         = ncc_tree_num_children(container);

    for (size_t i = 0; i < nc; i++) {
        if (ncc_tree_child(container, i) == wrapper) {
            insert_pos = i + 1;
            break;
        }
    }

    for (size_t i = 0; i < generated.len; i++) {
        add_trailing_newline(generated.items[i]);
        ncc_xform_insert_child(container, insert_pos + i, generated.items[i]);
    }

    node_list_free(&generated);
}

static char *
helper_source(ncc_sym_entry_t *entry, const char *name, const char *init_expr)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    char *type = ncc_type_of_symbol(entry);
    uint64_t typehash = type ? ncc_type_hash_u64(type) : 0;
    bool value_root = !symbol_type_is_pointer(entry);
    unsigned int value_scan_kind =
        (text_looks_like_array_value_initializer(init_expr)
         || text_looks_like_dict_value_initializer(init_expr)) ? 2u : 1u;
    static_addr_patch_list_t patches = {0};
    collect_static_addr_patch_markers(init_expr, &patches);

    ncc_buffer_printf(buf,
        "# line 1 \"ncc-generated-static-init-%s.c\"\n",
        name);

    if (value_root && text_uses_static_payload_allocator(init_expr)) {
        ncc_buffer_puts(buf,
            "extern void *n00b_crt_alloc_static_payload("
            "unsigned long long, unsigned long long, unsigned long long, "
            "unsigned int, n00b_gc_scan_cb_t, void *);\n");
    }
    if (value_root && strstr(init_expr, "n00b_crt_alloc_static_rwlock")) {
        ncc_buffer_puts(buf,
            "extern n00b_rwlock_t *n00b_crt_alloc_static_rwlock(void);\n");
    }

    ncc_buffer_printf(buf,
        "int __ncc_static_init_prepare_%s(void)\n"
        "{\n"
        "    typeof(%s) __ncc_static_init_value_%s = %s;\n"
        "    __builtin_memcpy((void *)&%s, "
        "&__ncc_static_init_value_%s, sizeof(%s));\n"
        "    return 0;\n"
        "}\n",
        name, name, name, init_expr, name, name, name);

    ncc_buffer_printf(buf,
        "typedef int (*__ncc_static_init_fn_%s)(void);\n"
        "#if defined(__APPLE__)\n"
        "[[gnu::used]] [[gnu::retain]] "
        "[[gnu::section(\"__DATA,n00b_sinit\")]]\n"
        "#elif defined(_WIN32)\n"
        "[[gnu::used]] [[gnu::section(\".n00bsi$m\")]]\n"
        "#else\n"
        "[[gnu::used]] [[gnu::retain]] [[gnu::section(\"n00b_sinit\")]]\n"
        "#endif\n"
        "static __ncc_static_init_fn_%s const "
        "__ncc_static_init_degrade_entry_%s = "
        "__ncc_static_init_prepare_%s;\n",
        name, name, name, name);

    if (value_root) {
        ncc_buffer_printf(buf,
            "void *__ncc_static_init_addr_%s(void)\n"
            "{\n"
            "    return (void *)&%s;\n"
            "}\n"
            "void __ncc_static_init_apply_%s(void *src)\n"
            "{\n"
            "    __builtin_memcpy((void *)&%s, src, sizeof(%s));\n"
            ,
            name, name,
            name, name, name);

        if (patches.len > 0) {
            ncc_buffer_printf(buf,
                "    __n00b_internal_type_erased_store_t *__ncc_patch_store =\n"
                "        (__n00b_internal_type_erased_store_t *)*(void *const *)&%s.store;\n",
                name);
            for (size_t i = 0; i < patches.len; i++) {
                ncc_buffer_printf(buf,
                    "    ((%s *)__ncc_patch_store->values)[%zu] = (%s)(%s);\n",
                    patches.items[i].value_type, patches.items[i].slot,
                    patches.items[i].value_type, patches.items[i].value_expr);
            }
        }

        ncc_buffer_printf(buf,
            "}\n"
            "unsigned long long __ncc_static_init_size_%s(void)\n"
            "{\n"
            "    return (unsigned long long)sizeof(%s);\n"
            "}\n"
            "unsigned long long __ncc_static_init_typehash_%s(void)\n"
            "{\n"
            "    return %lluULL;\n"
            "}\n"
            "unsigned int __ncc_static_init_scan_kind_%s(void)\n"
            "{\n"
            "    return %uu;\n"
            "}\n"
            "void *__ncc_static_init_scan_cb_%s(void)\n"
            "{\n"
            "    return 0;\n"
            "}\n"
            "void *__ncc_static_init_scan_user_%s(void)\n"
            "{\n"
            "    return 0;\n"
            "}\n",
            name, name,
            name, (unsigned long long)typehash,
            name, value_scan_kind,
            name,
            name);
    }

    ncc_buffer_puts(buf, "# line 1 \"ncc-generated-static-init-end.c\"\n");
    ncc_free(type);
    static_addr_patch_list_free(&patches);
    return ncc_buffer_take(buf);
}

static ncc_parse_tree_t *
xform_static_init_decl(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl)
{
    if (ncc_xform_find_ancestor(decl, "block_item")) {
        return nullptr;
    }

    ncc_parse_tree_t *list = ncc_xform_find_child_nt(
        decl, "init_declarator_list");
    if (!list) {
        return nullptr;
    }

    node_list_t init_decls = {0};
    collect_nt_children(list, "init_declarator", &init_decls);

    for (size_t i = 0; i < init_decls.len; i++) {
        ncc_parse_tree_t *init = ncc_xform_find_child_nt(
            init_decls.items[i], "initializer");
        if (!init) {
            continue;
        }

        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            init_decls.items[i], "declarator");
        char *name = declarator_name(declarator);
        ncc_sym_entry_t *entry = lookup_static_init_entry(ctx, name);
        if (!c_identifier_is_valid(name) || !entry) {
            ncc_free(name);
            continue;
        }

        char *init_expr = node_text(init);
        bool pointer_root = symbol_type_is_pointer(entry);
        const char *placeholder = pointer_root ? "nullptr" : "{}";
        if (strcmp(init_expr, placeholder) == 0
            || (strcmp(placeholder, "{}") == 0
                && text_is_empty_initializer(init_expr))) {
            ncc_free(init_expr);
            ncc_free(name);
            continue;
        }

        char *helpers   = helper_source(entry, name, init_expr);
        insert_external_decls_after(ctx, decl, helpers);

        ncc_parse_tree_t *replacement = ncc_xform_parse_source(
            ctx->grammar, "initializer", placeholder, "xform_static_init");
        if (!replacement) {
            fprintf(stderr,
                    "ncc: error: failed to parse static-init placeholder\n");
            exit(1);
        }
        replace_initializer(init_decls.items[i], replacement);

        ncc_free(helpers);
        ncc_free(init_expr);
        ncc_free(name);
    }

    node_list_free(&init_decls);
    return nullptr;
}

void
ncc_register_static_init_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "declaration", xform_static_init_decl,
                       "static_init");
}
