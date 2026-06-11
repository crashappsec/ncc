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
#include "util/platform.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WP-011 Phase 3c.i: precompute scalar-key dict hashes using the same
// XXH3_128bits over raw key bytes that the n00b runtime uses for
// `n00b_hash_raw(key_ptr, ksz)` (see /Users/viega/n00b/.../core/hash.c).
// We pull in xxhash as a static-inline implementation so the resulting
// 128-bit values are bit-identical to what the runtime computes during
// dict lookup; otherwise every static-dict lookup would fail.  Hashes
// are sent to the static-init helper via `hash <lo> <hi>` modifiers on
// the per-pair record (see helper's `arg pair cinit` parser).
//
// Conversion: XXH128_hash_t has fields { low64, high64 } and the n00b
// runtime's `n00b_xxh_convert` reinterprets the struct as a 128-bit
// integer.  Reading the union members in little-endian word order
// gives the same byte sequence; we expose `.low64` and `.high64`
// directly without needing the union trick.
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "vendor/xxhash.h"

typedef struct {
    char *object_type;
    char *elem_type;
} array_type_info_t;

typedef array_type_info_t list_type_info_t;

// WP-011 Phase 3c: dict type info table.  Mirrors `list_types`/`array_types`
// but records BOTH the key and value element types so the lowering pass
// can synthesize the helper request fields (`key_type_*`, `value_type_*`)
// for a `n00b_dict_t(K, V)` target.
typedef struct {
    char *object_type;
    char *key_type;
    char *value_type;
} dict_type_info_t;

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
static void collect_nt_children(ncc_parse_tree_t *node, const char *name,
                                ncc_array_t(ncc_parse_tree_ptr_t) *out);

static ncc_dict_t *
array_types(ncc_xform_ctx_t *ctx)
{
    return &ncc_xform_get_data(ctx)->array_types;
}

static ncc_dict_t *
list_types(ncc_xform_ctx_t *ctx)
{
    return &ncc_xform_get_data(ctx)->list_types;
}

static ncc_dict_t *
dict_types(ncc_xform_ctx_t *ctx)
{
    return &ncc_xform_get_data(ctx)->dict_types;
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

static void
record_list_type(ncc_xform_ctx_t *ctx, const char *key,
                 const char *object_type, const char *elem_type)
{
    if (!key || !*key || !object_type || !*object_type || !elem_type
        || !*elem_type) {
        return;
    }

    bool found = false;
    ncc_dict_get(list_types(ctx), (void *)key, &found);
    if (found) {
        return;
    }

    list_type_info_t *info = ncc_alloc(list_type_info_t);
    info->object_type      = copy_cstr(object_type);
    info->elem_type        = copy_cstr(elem_type);
    ncc_dict_put(list_types(ctx), copy_cstr(key), info);
}

static list_type_info_t *
lookup_list_type(ncc_xform_ctx_t *ctx, const char *key)
{
    if (!key || !*key) {
        return nullptr;
    }

    bool              found = false;
    list_type_info_t *info  = ncc_dict_get(list_types(ctx), (void *)key,
                                           &found);
    return found ? info : nullptr;
}

// WP-011 Phase 3c: dict type table helpers.  Mirror record_list_type/
// lookup_list_type but persist both key and value types so the lowering
// pass can produce key_type_*/value_type_* helper fields without re-
// inspecting the struct shape at lowering time.
static void
record_dict_type(ncc_xform_ctx_t *ctx, const char *key,
                 const char *object_type, const char *key_type,
                 const char *value_type)
{
    if (!key || !*key || !object_type || !*object_type
        || !key_type || !*key_type
        || !value_type || !*value_type) {
        return;
    }

    bool found = false;
    ncc_dict_get(dict_types(ctx), (void *)key, &found);
    if (found) {
        return;
    }

    dict_type_info_t *info = ncc_alloc(dict_type_info_t);
    info->object_type      = copy_cstr(object_type);
    info->key_type         = copy_cstr(key_type);
    info->value_type       = copy_cstr(value_type);
    ncc_dict_put(dict_types(ctx), copy_cstr(key), info);
}

static dict_type_info_t *
lookup_dict_type(ncc_xform_ctx_t *ctx, const char *key)
{
    if (!key || !*key) {
        return nullptr;
    }

    bool              found = false;
    dict_type_info_t *info  = ncc_dict_get(dict_types(ctx), (void *)key,
                                           &found);
    return found ? info : nullptr;
}

// WP-011 Phase 3c.iii: produce a user-friendly dict type label for
// diagnostics.  The `object_type` field is the post-typeid-mangle
// `struct n00b_dict_<HASH>` form, which is stable but unreadable.
// Phase 3c.iii synthesizes `n00b_dict_t(<K>, <V>)` from the recorded
// K and V types so error messages name the type the way the user
// wrote it (or a close paraphrase) instead of leaking the mangled
// runtime tag or the `_generic_struct typeid(...)` expansion.  The
// returned buffer is owned by the caller; free with `ncc_free`.
static char *
dict_type_friendly_name(const dict_type_info_t *info)
{
    if (!info) {
        return copy_cstr("<unknown dict>");
    }
    const char *k = info->key_type   ? info->key_type   : "?";
    const char *v = info->value_type ? info->value_type : "?";
    size_t      len = strlen(k) + strlen(v) + 32;
    char       *out = ncc_alloc_size(1, len);
    snprintf(out, len, "n00b_dict_t(%s, %s)", k, v);
    return out;
}

// WP-011 Phase 3c.iv: parallel friendly-name helpers for list and
// array diagnostics.  `object_type` is the post-typeid-mangle
// `struct n00b_list_<HASH>` / `struct ncc_array_<HASH>` form, which
// is stable but unreadable.  We synthesize `n00b_list_t(<elem>)` /
// `n00b_array_t(<elem>)` from the recorded element type so error
// messages name the type the way the user wrote it (or a close
// paraphrase) instead of leaking the mangled runtime tag or the
// `_generic_struct typeid(...)` expansion.  Both forms are accepted
// list/array spellings in user code (the array spelling has two
// equivalent surface forms `ncc_array_t(T)` and `n00b_array_t(T)`;
// we standardize on `n00b_array_t(T)` to match the existing
// diagnostic text in target-mismatch errors).  Returned buffers
// are owned by the caller; free with `ncc_free`.
static char *
list_type_friendly_name(const list_type_info_t *info)
{
    if (!info) {
        return copy_cstr("<unknown list>");
    }
    const char *elem = info->elem_type ? info->elem_type : "?";
    size_t      len  = strlen(elem) + 32;
    char       *out  = ncc_alloc_size(1, len);
    snprintf(out, len, "n00b_list_t(%s)", elem);
    return out;
}

static char *
array_type_friendly_name(const array_type_info_t *info)
{
    if (!info) {
        return copy_cstr("<unknown array>");
    }
    const char *elem = info->elem_type ? info->elem_type : "?";
    size_t      len  = strlen(elem) + 32;
    char       *out  = ncc_alloc_size(1, len);
    snprintf(out, len, "n00b_array_t(%s)", elem);
    return out;
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

static bool
tag_is_list_type(ncc_parse_tree_t *tag_node)
{
    char *tag = node_text(tag_node);
    bool  result = false;

    if (strncmp(tag, "n00b_list_", 10) == 0) {
        result = true;
    }
    else if (strstr(tag, "typeid") && strstr(tag, "\"n00b_list\"")) {
        result = true;
    }

    ncc_free(tag);
    return result;
}

// WP-011 Phase 2: dict-target tag detection.  Mirrors the list/array
// `tag_is_*_type` shape: matches the runtime tag prefix `n00b_dict_`
// (post-typeid-mangling) and the pre-mangle `typeid("n00b_dict", ...)`
// form.  Phase 2 only needs to know "this target is a dict"; Phase 3
// will record the resolved K/V types for the helper request.
static bool
tag_is_dict_type(ncc_parse_tree_t *tag_node)
{
    char *tag    = node_text(tag_node);
    bool  result = false;

    if (strncmp(tag, "n00b_dict_", 10) == 0) {
        result = true;
    }
    else if (strstr(tag, "typeid") && strstr(tag, "\"n00b_dict\"")) {
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

static list_type_info_t *
list_type_from_struct_spec(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su)
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
    bool  is_list   = elem_type && tag_is_list_type(tag);

    if (!is_list) {
        list_type_info_t *known = lookup_list_type(ctx, object_type);
        ncc_free(runtime_tag);
        ncc_free(object_type);
        ncc_free(elem_type);
        return known;
    }

    record_list_type(ctx, object_type, object_type, elem_type);
    record_list_type(ctx, runtime_tag, object_type, elem_type);

    list_type_info_t *result = lookup_list_type(ctx, object_type);
    ncc_free(runtime_tag);
    ncc_free(object_type);
    ncc_free(elem_type);
    return result;
}

// WP-011 Phase 3c: extract the K and V type names from a dict tag's
// `typeid("n00b_dict", K, V)` synthetic identifier.  The `<tag_name>`
// of `n00b_dict_t(K, V)` (post-`_generic_struct` expansion) holds a
// `<synthetic_identifier>` whose first `<typeid_atom>` is the string
// literal `"n00b_dict"` and whose `<typeid_continuation>` chain
// carries the K and V type names.
//
// This avoids depending on the user-visible struct layout: the test
// fixtures and the real libn00b struct both store keys/values inside
// the dict's `store`, not as top-level members.  The typeid is the
// source of truth for K and V.
static char *
typeid_continuation_nth_atom(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *cont,
                             size_t index)
{
    // index 0 → first atom in the continuation chain (i.e., the
    // second `typeid_atom` overall, the one after the "n00b_dict"
    // header).  index 1 → the next one.
    while (cont) {
        if (!ncc_xform_nt_name_is(cont, "typeid_continuation")) {
            // Unwrap group wrappers.
            ncc_parse_tree_t *inner = ncc_xform_find_child_nt(
                cont, "typeid_continuation");
            if (!inner || inner == cont) {
                return nullptr;
            }
            cont = inner;
            continue;
        }

        ncc_parse_tree_t *atom = ncc_xform_find_child_nt(cont, "typeid_atom");
        if (index == 0) {
            if (!atom) {
                return nullptr;
            }
            ncc_string_t s = ncc_xform_extract_type_string(ctx, atom, nullptr);
            char *result = trim_copy(s.data);
            ncc_free(s.data);
            return result;
        }

        ncc_parse_tree_t *next = ncc_xform_find_child_nt(cont,
                                                         "typeid_continuation");
        if (!next || next == cont) {
            return nullptr;
        }
        cont = next;
        index--;
    }
    return nullptr;
}

static bool
dict_kv_from_tag(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tag_node,
                 char **out_key, char **out_value)
{
    *out_key   = nullptr;
    *out_value = nullptr;

    ncc_parse_tree_t *synthetic = first_descendant_nt(tag_node,
                                                      "synthetic_identifier");
    if (!synthetic || !contains_leaf_text(synthetic, "typeid")) {
        return false;
    }

    ncc_parse_tree_t *cont = ncc_xform_find_child_nt(synthetic,
                                                     "typeid_continuation");
    if (!cont) {
        return false;
    }

    char *key   = typeid_continuation_nth_atom(ctx, cont, 0);
    char *value = typeid_continuation_nth_atom(ctx, cont, 1);
    if (!key || !value) {
        ncc_free(key);
        ncc_free(value);
        return false;
    }

    *out_key   = key;
    *out_value = value;
    return true;
}

// WP-011 Phase 3c: resolve a `n00b_dict_t(K, V)` from its generic
// struct specifier.  Records both the key and value element types so
// the lowering pass can build helper requests without re-inspecting
// the struct.  K and V come from the tag's `typeid("n00b_dict", K, V)`
// synthetic identifier rather than from member declarations — the
// real libn00b dict struct stores keys/values inside its `store`, not
// as top-level members, so the typeid is the only stable source.
static dict_type_info_t *
dict_type_from_struct_spec(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *tag = ncc_xform_find_child_nt(su, "tag_name");
    if (!tag) {
        return nullptr;
    }

    char *runtime_tag = tag_runtime_name(ctx, tag);

    ncc_buffer_t *obj_buf = ncc_buffer_empty();
    ncc_buffer_printf(obj_buf, "struct %s", runtime_tag);
    char *object_type = ncc_buffer_take(obj_buf);

    char *key_type   = nullptr;
    char *value_type = nullptr;
    bool  have_kv    = dict_kv_from_tag(ctx, tag, &key_type, &value_type);
    bool  is_dict    = have_kv && tag_is_dict_type(tag);

    if (!is_dict) {
        dict_type_info_t *known = lookup_dict_type(ctx, object_type);
        ncc_free(runtime_tag);
        ncc_free(object_type);
        ncc_free(key_type);
        ncc_free(value_type);
        return known;
    }

    record_dict_type(ctx, object_type, object_type, key_type, value_type);
    record_dict_type(ctx, runtime_tag, object_type, key_type, value_type);

    dict_type_info_t *result = lookup_dict_type(ctx, object_type);
    ncc_free(runtime_tag);
    ncc_free(object_type);
    ncc_free(key_type);
    ncc_free(value_type);
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

static list_type_info_t *
list_type_from_decl_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl_specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(decl_specs,
                                               "struct_or_union_specifier");
    if (su) {
        list_type_info_t *info = list_type_from_struct_spec(ctx, su);
        if (info) {
            return info;
        }
    }

    char *name = decl_specs_typedef_name(decl_specs);
    list_type_info_t *info = lookup_list_type(ctx, name);
    ncc_free(name);
    return info;
}

// WP-011 Phase 3c: dict target resolution.  Mirrors
// `list_type_from_decl_specs`: inspect the inline struct-or-union
// specifier first (for `n00b_dict_t(K, V) x = ...` declarations), then
// fall back to a typedef-alias lookup so `typedef n00b_dict_t(int, int)
// my_dict_t; my_dict_t y = ...` also resolves.  The typedef-alias path
// relies on `record_dict_typedef_aliases` having recorded the alias
// when the typedef declaration itself was processed.
static dict_type_info_t *
dict_type_from_decl_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl_specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(decl_specs,
                                               "struct_or_union_specifier");
    if (su) {
        dict_type_info_t *info = dict_type_from_struct_spec(ctx, su);
        if (info) {
            return info;
        }
    }

    char *name = decl_specs_typedef_name(decl_specs);
    dict_type_info_t *info = lookup_dict_type(ctx, name);
    ncc_free(name);
    return info;
}

// WP-011 Phase 3c: dict-target detection is folded into
// `dict_type_from_decl_specs` — callers check whether the returned
// info pointer is non-null.  Phase 2's standalone
// `decl_specs_target_is_dict` is removed because the lowering pass
// needs the resolved K/V types anyway, not just a boolean.

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
first_child_leaf(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }
    if (ncc_tree_is_leaf(node)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found = first_child_leaf(ncc_tree_child(node, i));
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static ncc_parse_tree_t *
initializer_modified_literal(ncc_parse_tree_t *initializer)
{
    if (!initializer) {
        return nullptr;
    }
    return ncc_xform_find_child_nt(initializer, "modified_literal");
}

static const char *
modified_literal_modifier(ncc_parse_tree_t *literal)
{
    if (!literal || !ncc_xform_nt_name_is(literal, "modified_literal")) {
        return nullptr;
    }

    ncc_parse_tree_t *leaf = first_child_leaf(literal);
    return leaf ? ncc_xform_leaf_text(leaf) : nullptr;
}

static bool
modified_literal_is(ncc_parse_tree_t *literal, const char *modifier)
{
    const char *got = modified_literal_modifier(literal);
    return got && modifier && strcmp(got, modifier) == 0;
}

static ncc_parse_tree_t *
initializer_array_literal(ncc_parse_tree_t *initializer)
{
    if (!initializer) {
        return nullptr;
    }

    ncc_parse_tree_t *literal = ncc_xform_find_child_nt(initializer,
                                                        "array_literal");
    if (literal) {
        return literal;
    }

    literal = initializer_modified_literal(initializer);
    return modified_literal_is(literal, "a") ? literal : nullptr;
}

static ncc_parse_tree_t *
initializer_list_literal(ncc_parse_tree_t *initializer)
{
    if (!initializer) {
        return nullptr;
    }
    ncc_parse_tree_t *literal = initializer_modified_literal(initializer);
    return modified_literal_is(literal, "l") ? literal : nullptr;
}

// WP-011 Phase 2: dict-literal initializer detection.  Recognizes
// both spellings:
//   1) explicit `d{...}` — parsed as <modified_literal> with modifier "d";
//   2) bare `{key: value, ...}` — parsed as <dict_literal>.
// Bare `{}`, `{1,2,3}`, and `{.x = 5}` stay <braced_initializer>; the
// grammar's <dict_literal> requires at least one <dict_pair> so the
// empty / value-only / designated-initializer forms cannot match it.
// Empty static dicts are spelled `d{}` and reach the dispatch via the
// <modified_literal> branch.
static ncc_parse_tree_t *
initializer_dict_literal(ncc_parse_tree_t *initializer)
{
    if (!initializer) {
        return nullptr;
    }

    // Bare `{key: value, ...}` form.
    ncc_parse_tree_t *bare = ncc_xform_find_child_nt(initializer,
                                                     "dict_literal");
    if (bare) {
        return bare;
    }

    // Explicit `d{...}` form (modifier == "d").
    ncc_parse_tree_t *modified = initializer_modified_literal(initializer);
    return modified_literal_is(modified, "d") ? modified : nullptr;
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
    // Delegate to the shared symtab-backed resolver (single source of truth).
    return ncc_layout_lookup_aggregate_type(ctx, key);
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
offset_expr_for_pointer_array_item(const char *type_name,
                                   const char *field_path, uint64_t ix)
{
    if (ix == 0) {
        return offset_expr_for_field_path(type_name, field_path);
    }

    return format_cstr(
        "((uint64_t)((__builtin_offsetof(%s,%s)/sizeof(void*))+%lluULL))",
        type_name, field_path, (unsigned long long)ix);
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
                              int dim_index, uint64_t linear_ix,
                              uint64_list_t *dims)
{
    (void)ctx;
    if ((size_t)dim_index == dims->len) {
        list_push(offsets, offset_expr_for_pointer_array_item(elem_type,
                                                              field_path,
                                                              linear_ix));
        list_push(asserts, offset_assert_for_field_path(elem_type,
                                                        field_path));
        return;
    }

    uint64_t stride = 1;
    for (size_t i = (size_t)dim_index + 1; i < dims->len; i++) {
        stride *= dims->data[i];
    }

    for (uint64_t i = 0; i < dims->data[dim_index]; i++) {
        collect_pointer_array_offsets(ctx, site, elem_type, field_path,
                                      declarator, offsets, asserts,
                                      dim_index + 1,
                                      linear_ix + i * stride, dims);
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
                                          0, &dims);
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
    // WP-011 Phase 5a (finding 2): the configured `rstr_string_type`
    // is usually the *value* type (no `*`), but typeid-extracted dict
    // key types arrive with the `*` already attached (e.g.
    // `n00b_string_t *`). Whitespace gets compacted out, but the
    // trailing `*` does not — so we also accept
    // `<compacted rstr_string_type>*` as a match. The existing fixed
    // `_ptr` / `_t_ptr` suffix forms stay; this is purely additive.
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    char *compact = compact_type(type);
    char *rstr = compact_type(data->rstr_string_type);
    size_t rstr_len = strlen(rstr);
    char *rstr_ptr = ncc_alloc_size(1, rstr_len + 2);
    memcpy(rstr_ptr, rstr, rstr_len);
    rstr_ptr[rstr_len]     = '*';
    rstr_ptr[rstr_len + 1] = '\0';
    bool result = strcmp(compact, rstr) == 0
        || strcmp(compact, rstr_ptr) == 0
        || strcmp(compact, "ncc_string_ptr_t") == 0
        || strcmp(compact, "ncc_string_t_ptr") == 0
        || strcmp(compact, "n00b_string_ptr_t") == 0
        || strcmp(compact, "n00b_string_t_ptr") == 0;
    ncc_free(rstr_ptr);
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
append_hex(ncc_buffer_t *out, const void *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *bytes = data;

    for (size_t i = 0; i < len; i++) {
        ncc_buffer_putc(out, hex[bytes[i] >> 4]);
        ncc_buffer_putc(out, hex[bytes[i] & 0xf]);
    }
}

static char *
hex_string(const void *data, size_t len)
{
    ncc_buffer_t *out = ncc_buffer_empty();
    append_hex(out, data, len);
    return ncc_buffer_take(out);
}

static const char *
static_image_host_endian_value(void)
{
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) \
    && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return "2u";
#else
    return "1u";
#endif
}

static void
literal_helper_error(ncc_parse_tree_t *site, const char *literal_kind,
                     const char *type_name, const char *message,
                     const char *stderr_data, size_t stderr_len)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf, "%s static initializer for '%s' %s",
                      literal_kind ? literal_kind : "literal",
                      type_name ? type_name : "<unknown>", message);
    if (stderr_data && stderr_len) {
        ncc_buffer_puts(buf, ": ");
        ncc_buffer_append(buf, stderr_data, stderr_len);
    }

    char *text = ncc_buffer_take(buf);
    array_errorf(site, "%s", text);
}

static void
run_literal_static_init_helper(ncc_parse_tree_t *site, const char *helper,
                               const char *literal_kind,
                               const char *type_name, const char *request,
                               char **expr_out, char **decls_out)
{
    *expr_out  = nullptr;
    *decls_out = nullptr;

    const char *argv[] = {helper, nullptr};
    ncc_process_spec_t spec = {
        .program        = helper,
        .argv           = argv,
        .stdin_data     = request,
        .stdin_len      = strlen(request),
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};

    if (!ncc_process_run(&spec, &proc)) {
        literal_helper_error(site, literal_kind, type_name,
                             "could not be launched", proc.stderr_data,
                             proc.stderr_len);
    }

    if (proc.exit_code != 0) {
        literal_helper_error(site, literal_kind, type_name, "failed",
                             proc.stderr_data, proc.stderr_len);
    }

    const char prefix[] = "NCC_STATIC_INIT_OK ";
    if (!proc.stdout_data
        || proc.stdout_len < sizeof(prefix) - 1
        || strncmp(proc.stdout_data, prefix, sizeof(prefix) - 1) != 0) {
        literal_helper_error(site, literal_kind, type_name,
                             "returned an invalid response", proc.stderr_data,
                             proc.stderr_len);
    }

    char *newline = memchr(proc.stdout_data, '\n', proc.stdout_len);
    if (!newline || newline == proc.stdout_data + sizeof(prefix) - 1) {
        literal_helper_error(site, literal_kind, type_name,
                             "returned an invalid response", proc.stderr_data,
                             proc.stderr_len);
    }

    size_t expr_len = (size_t)(newline - proc.stdout_data)
                    - (sizeof(prefix) - 1);
    char *expr = ncc_alloc_array(char, expr_len + 1);
    memcpy(expr, proc.stdout_data + sizeof(prefix) - 1, expr_len);
    expr[expr_len] = '\0';

    size_t decl_len = proc.stdout_len
                    - (size_t)(newline + 1 - proc.stdout_data);
    char *decls = ncc_alloc_array(char, decl_len + 1);
    memcpy(decls, newline + 1, decl_len);
    decls[decl_len] = '\0';

    *expr_out  = expr;
    *decls_out = decls;

    ncc_process_result_free(&proc);
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

// WP-018 / WP-016 B1: defense-in-depth bound on container-literal
// nesting depth. Hostile or pathological source like `[[[[...]]]]`
// otherwise recurses through `lower_array_literal` /
// `lower_list_literal` / `lower_dict_literal` until ncc blows the
// stack. The cap matches `collect_static_layout_offsets_for_aggregate`'s
// 64-level limit. The counter is `thread_local` so it stays correct
// if ncc ever fans out compilation across threads; today ncc is
// single-threaded per source file, so this is purely defensive.
//
// All three `lower_*_literal` entry points share one counter because
// they freely call each other (an array of lists, a list of dicts, a
// dict of arrays, etc.); the limit is on total container nesting at
// the literal site, not on each kind separately.
#define NCC_STATIC_CONTAINER_LITERAL_MAX_DEPTH 64
static thread_local int ncc_static_container_literal_depth = 0;

static void
ncc_static_container_literal_enter(ncc_parse_tree_t *literal,
                                   const char *kind)
{
    if (ncc_static_container_literal_depth
        >= NCC_STATIC_CONTAINER_LITERAL_MAX_DEPTH) {
        array_errorf(literal,
                     "%s literal exceeds the maximum static-initializer "
                     "nesting depth (%d levels); flatten the literal or "
                     "split it into multiple declarations",
                     kind, NCC_STATIC_CONTAINER_LITERAL_MAX_DEPTH);
        // array_errorf does not return.
    }
    ncc_static_container_literal_depth++;
}

static void
ncc_static_container_literal_leave(void)
{
    if (ncc_static_container_literal_depth > 0) {
        ncc_static_container_literal_depth--;
    }
}

static char *lower_list_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                                ncc_parse_tree_t *literal,
                                list_type_info_t *type, string_list_t *decls,
                                bool readonly, bool pointer_target);

// WP-011 Phase 3c.i: full scalar/enum-keyed dict literal lowering and
// a partial-stub branch for pointer-keyed dicts (Phase 3c.ii).
// `dict_type_info_t` is defined near the top of this file alongside
// `array_type_info_t` / `list_type_info_t`.
static char *lower_dict_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                                ncc_parse_tree_t *literal,
                                dict_type_info_t *type, string_list_t *decls,
                                bool readonly, bool pointer_target);

static char *
build_array_initializer(const char *data_name, int count,
                        const char *scan_kind, const char *scan_cb,
                        const char *scan_user)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "{.data=%s,.len=%d,.cap=%d,.lock=nullptr,"
                      ".allocator=nullptr,.scan_kind=%s,.scan_cb=%s,"
                      ".scan_user=%s}",
                      data_name ? data_name : "nullptr", count, count,
                      scan_kind ? scan_kind : "1",
                      scan_cb ? scan_cb : "nullptr",
                      scan_user ? scan_user : "nullptr");
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

static bool
is_buflit_call_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "postfix_expression")
        || ncc_tree_num_children(node) < 3
        || !ncc_xform_leaf_text_eq(ncc_tree_child(node, 1), "(")) {
        return false;
    }

    const char *name = ncc_xform_get_first_leaf_text(ncc_tree_child(node, 0));
    return name && strcmp(name, "__ncc_buflit") == 0;
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

static void
collect_buflit_calls(ncc_parse_tree_t *node,
                     ncc_array_t(ncc_parse_tree_ptr_t) *calls)
{
    if (!node) {
        return;
    }

    if (is_buflit_call_node(node)) {
        parse_tree_array_push(calls, node);
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_buflit_calls(ncc_tree_child(node, i), calls);
    }
}

static bool
string_literal_leaf(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    return len >= 2 && text[0] == '"' && text[len - 1] == '"';
}

static void
decode_string_leaf(ncc_buffer_t *buf, const char *text)
{
    const char *p   = text + 1;
    const char *end = text + strlen(text) - 1;

    while (p < end) {
        if (*p != '\\' || p + 1 >= end) {
            ncc_buffer_putc(buf, *p++);
            continue;
        }

        p++;
        switch (*p) {
        case 'n':
            ncc_buffer_putc(buf, '\n');
            p++;
            break;
        case 'r':
            ncc_buffer_putc(buf, '\r');
            p++;
            break;
        case 't':
            ncc_buffer_putc(buf, '\t');
            p++;
            break;
        case '0':
            ncc_buffer_putc(buf, '\0');
            p++;
            break;
        case '\\':
            ncc_buffer_putc(buf, '\\');
            p++;
            break;
        case '"':
            ncc_buffer_putc(buf, '"');
            p++;
            break;
        case 'a':
            ncc_buffer_putc(buf, '\a');
            p++;
            break;
        case 'b':
            ncc_buffer_putc(buf, '\b');
            p++;
            break;
        case 'f':
            ncc_buffer_putc(buf, '\f');
            p++;
            break;
        case 'v':
            ncc_buffer_putc(buf, '\v');
            p++;
            break;
        case 'x': {
            p++;
            unsigned int val = 0;
            for (int d = 0; d < 2 && p < end; d++, p++) {
                unsigned char c = (unsigned char)*p;
                if (c >= '0' && c <= '9') {
                    val = val * 16 + (c - '0');
                }
                else if (c >= 'a' && c <= 'f') {
                    val = val * 16 + (c - 'a' + 10);
                }
                else if (c >= 'A' && c <= 'F') {
                    val = val * 16 + (c - 'A' + 10);
                }
                else {
                    break;
                }
            }
            ncc_buffer_putc(buf, (char)val);
            break;
        }
        default:
            ncc_buffer_putc(buf, '\\');
            ncc_buffer_putc(buf, *p++);
            break;
        }
    }
}

static bool
decode_buflit_call_strings(ncc_parse_tree_t *node, ncc_buffer_t *out,
                           int *string_count)
{
    if (!node) {
        return true;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (!text) {
            return true;
        }
        if (string_literal_leaf(text)) {
            decode_string_leaf(out, text);
            (*string_count)++;
        }
        return true;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (!decode_buflit_call_strings(ncc_tree_child(node, i), out,
                                        string_count)) {
            return false;
        }
    }

    return true;
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
            ncc_free(rstr.content);
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
            ncc_free(rstr.content);
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
        ncc_free(rstr.content);
    }

    ncc_array_free(calls);
    return true;
}

// WP-011 Phase 5f: forward declaration so `lower_buffer_literal_ref`
// (the non-dict-key emission path) can precompute the descriptor's
// `.cached_hash` slot without re-ordering the existing definition
// further down the file (the dict-key path at ~line 4895 also uses
// this helper, so its current position is load-bearing for that
// caller too).
static void
compute_buffer_key_hash(const unsigned char *bytes, size_t len,
                        uint64_t *out_lo, uint64_t *out_hi);

// WP-011 Phase 3c.ii.b / 5f: every b"..." literal emission threads
// its XXH3_128bits through two named integer args
// (`cached_hash_lo`, `cached_hash_hi`) so the n00b-side
// `n00b_buffer_static_init` builder can populate the
// `n00b_buffer_t` object descriptor's `.cached_hash` slot at build
// time.  Phase 3c.ii.b introduced this for dict-key buffer literals;
// Phase 5f extended it to every buffer literal emission (standalone,
// list/array/struct element) via `lower_buffer_literal_ref` so
// content-equal `b"..."` literals share the same cached_hash across
// every occurrence — the symmetric fix to Phase 5d's r-string
// cached_hash population (D-066 perf path).  For an empty `b""`
// literal both halves are zero and the descriptor keeps
// `.cached_hash = 0` (algorithm parity with `n00b_buffer_hash`'s
// zero-length `n00b_hash_word(0ULL)` fallback, which we can't
// reproduce at ncc compile time).
static char *
build_buffer_literal_helper_request(ncc_xform_ctx_t *ctx,
                                    ncc_parse_tree_t *call,
                                    const char *prefix,
                                    const char *payload,
                                    size_t payload_len,
                                    uint64_t cached_hash_lo,
                                    uint64_t cached_hash_hi)
{
    const char *type_name = "n00b_buffer_t";
    char *type_hex = hex_string(type_name, strlen(type_name));
    char *payload_hex = hex_string(payload, payload_len);
    char *type_hash = ncc_static_object_typehash_expr("n00b_buffer_t*");
    const char *entry_attr = ncc_static_object_entry_attr(ctx);
    char *entry_attr_hex = hex_string(entry_attr, strlen(entry_attr));
    char *identity_namespace =
        ncc_static_object_identity_namespace(ctx, call);
    char *identity_object_key =
        ncc_static_object_identity_key(ctx, "ncc-static-image-object",
                                       call, type_hash, "1");
    char *identity_payload_key =
        ncc_static_object_identity_key(ctx, "ncc-static-image-payload",
                                       call, "0", "payload");
    char *identity_namespace_hex =
        hex_string(identity_namespace, strlen(identity_namespace));
    char *identity_object_key_hex =
        hex_string(identity_object_key, strlen(identity_object_key));
    char *identity_payload_key_hex =
        hex_string(identity_payload_key, strlen(identity_payload_key));

    // The helper's arg-parser accepts integer values as signed
    // (`parse_i64_token`).  We always have well-defined 64-bit values
    // here; cast through `int64_t` so the helper's value matches the
    // unsigned bit pattern (two's-complement reinterpretation).  The
    // n00b-side builder casts back to uint64_t before reassembling
    // the 128-bit cached_hash.
    bool emit_cached_hash = (cached_hash_lo != 0) || (cached_hash_hi != 0);
    int  base_arg_count   = 1;
    int  total_args       = base_arg_count + (emit_cached_hash ? 2 : 0);

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "NCC_STATIC_INIT 1\n"
                      "type_hex %s\n"
                      "type_hash %s\n"
                      "prefix %s\n"
                      "readonly 1\n"
                      "abi %zu %zu 8 %s\n"
                      "entry_attr_hex %s\n"
                      "identity_namespace_hex %s\n"
                      "identity_object_key_hex %s\n"
                      "identity_payload_key_hex %s\n"
                      "arg_count %d\n"
                      "arg - bytes %zu %s\n",
                      type_hex, type_hash, prefix, sizeof(void *),
                      sizeof(size_t), static_image_host_endian_value(),
                      entry_attr_hex, identity_namespace_hex,
                      identity_object_key_hex, identity_payload_key_hex,
                      total_args, payload_len, payload_hex);

    if (emit_cached_hash) {
        ncc_buffer_printf(buf,
                          "arg cached_hash_lo int %lld\n"
                          "arg cached_hash_hi int %lld\n",
                          (long long)(int64_t)cached_hash_lo,
                          (long long)(int64_t)cached_hash_hi);
    }

    ncc_buffer_puts(buf, "end\n");

    ncc_free(type_hex);
    ncc_free(payload_hex);
    ncc_free(type_hash);
    ncc_free(entry_attr_hex);
    ncc_free(identity_namespace);
    ncc_free(identity_object_key);
    ncc_free(identity_payload_key);
    ncc_free(identity_namespace_hex);
    ncc_free(identity_object_key_hex);
    ncc_free(identity_payload_key_hex);
    return ncc_buffer_take(buf);
}

// WP-011 Phase 3c.ii.b / 5f: shared lowering helper for b"..."
// literals.  Both callers (`lower_buffer_literal_ref` for the common
// non-key path post-Phase-5f, and the dict-key path that calls
// `lower_buffer_literal_ref_with_hash` directly) precompute the
// payload's XXH3_128bits and thread the value here so the emitted
// buffer object descriptor's `.cached_hash` slot lands the same
// XXH3 the runtime would recompute on first lookup.  Empty buffer
// payloads pass `(0, 0)` — the helper's `n00b_buffer_static_init`
// keeps `.cached_hash = 0` and the runtime falls back to
// `n00b_hash_word(0ULL)` on the zero-length branch (caveat
// documented at `compute_buffer_key_hash` / `lower_buffer_literal_ref`).
//
// `out_payload_bytes` / `out_payload_len` (if non-NULL) receive a
// freshly allocated copy of the literal's decoded bytes so dict-key
// callers can re-feed them to `compute_buffer_key_hash` without re-
// parsing the call.  Caller owns the allocation; free with
// `ncc_free`.  Pass `nullptr`/`nullptr` to ignore.
static char *
lower_buffer_literal_ref_with_hash(ncc_xform_ctx_t *ctx,
                                   ncc_parse_tree_t *call,
                                   string_list_t *decls,
                                   uint64_t cached_hash_lo,
                                   uint64_t cached_hash_hi,
                                   unsigned char **out_payload_bytes,
                                   size_t *out_payload_len)
{
    ncc_buffer_t *payload = ncc_buffer_empty();
    int string_count = 0;
    if (!decode_buflit_call_strings(call, payload, &string_count)
        || string_count == 0) {
        ncc_buffer_free(payload);
        array_error(call, "could not decode b-string list element", nullptr);
    }

    if (out_payload_bytes && out_payload_len) {
        size_t plen = payload->byte_len;
        unsigned char *copy = nullptr;
        if (plen > 0) {
            copy = ncc_alloc_size(1, plen);
            memcpy(copy, payload->data, plen);
        }
        *out_payload_bytes = copy;
        *out_payload_len   = plen;
    }

    int id = ctx->unique_id++;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__ncc_buflit_%d", id);

    char *request = build_buffer_literal_helper_request(
        ctx, call, prefix, payload->data, payload->byte_len,
        cached_hash_lo, cached_hash_hi);
    ncc_buffer_free(payload);

    const char *helper = ncc_xform_get_data(ctx)->static_init_helper;
    if (!helper || !*helper) {
        ncc_free(request);
        array_errorf(call,
                     "b\"...\" buffer list element requires "
                     "--ncc-static-init-helper=PATH");
    }

    char *expr_name = nullptr;
    char *helper_decls = nullptr;
    run_literal_static_init_helper(call, helper, "buffer literal",
                                   "n00b_buffer_t", request,
                                   &expr_name, &helper_decls);
    list_push(decls, helper_decls);
    ncc_free(request);
    return expr_name;
}

// WP-011 Phase 5f: every non-dict-key buffer literal emission now
// populates the descriptor's `.cached_hash` slot with the same
// XXH3_128bits the runtime `n00b_buffer_hash` would compute — see
// `compute_buffer_key_hash` above for the algorithm parity contract
// and the empty-buffer caveat (an empty `b""` keeps cached_hash = 0
// because `n00b_buffer_hash` falls back to `n00b_hash_word(0ULL)` on
// the zero-length branch, which we cannot reproduce at ncc compile
// time without depending on libn00b's `n00b_word_t` layout).  This is
// the symmetric fix to Phase 5d (r-string cached_hash population); it
// ensures `n00b_hash(buffer_ptr)` returns the same value at every
// occurrence of a content-equal `b"..."` literal across the program,
// realising D-066's runtime perf path for buffer keys too.
//
// We pre-decode the payload once to compute the hash, then call
// `lower_buffer_literal_ref_with_hash` which decodes a second time
// internally; the cost is tiny and the shape stays simple (same
// double-decode pattern the dict-key path uses).
static char *
lower_buffer_literal_ref(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *call,
                         string_list_t *decls)
{
    uint64_t cached_hash_lo = 0;
    uint64_t cached_hash_hi = 0;

    ncc_buffer_t *probe = ncc_buffer_empty();
    int probe_count = 0;
    if (decode_buflit_call_strings(call, probe, &probe_count)
        && probe_count > 0 && probe->byte_len > 0) {
        compute_buffer_key_hash((const unsigned char *)probe->data,
                                probe->byte_len,
                                &cached_hash_lo, &cached_hash_hi);
    }
    // Empty buffers (probe->byte_len == 0) keep both halves at 0; the
    // descriptor's `.cached_hash` slot stays zero and the runtime
    // `n00b_buffer_hash` falls back to `n00b_hash_word(0ULL)` on first
    // lookup.  Decode failure is also tolerated: we let the second
    // decode inside `lower_buffer_literal_ref_with_hash` produce the
    // canonical error so the caller sees a consistent diagnostic.
    ncc_buffer_free(probe);

    return lower_buffer_literal_ref_with_hash(ctx, call, decls,
                                              cached_hash_lo,
                                              cached_hash_hi,
                                              nullptr, nullptr);
}

static bool
is_buffer_pointer_element_type(const char *type)
{
    char *compact = compact_type(type);
    bool  result = strcmp(compact, "n00b_buffer_t*") == 0
                || strcmp(compact, "n00b_buffer_ptr_t") == 0
                || strcmp(compact, "n00b_buffer_t_ptr") == 0;
    ncc_free(compact);
    return result;
}

static bool
lower_buffers_in_initializer_expr(ncc_xform_ctx_t *ctx,
                                  ncc_parse_tree_t *init,
                                  string_list_t *decls,
                                  char **expr_inout)
{
    ncc_array_t(ncc_parse_tree_ptr_t) calls =
        ncc_array_new(ncc_parse_tree_ptr_t, 4);
    collect_buflit_calls(init, &calls);

    if (calls.len == 0) {
        ncc_array_free(calls);
        return false;
    }

    for (size_t i = 0; i < calls.len; i++) {
        char *call_text = node_text(calls.data[i]);
        char *buffer_expr = lower_buffer_literal_ref(ctx, calls.data[i],
                                                     decls);
        char *rewritten = replace_first_substr(*expr_inout, call_text,
                                               buffer_expr);
        if (!rewritten) {
            ncc_free(call_text);
            ncc_free(buffer_expr);
            ncc_array_free(calls);
            array_error(init,
                        "could not splice generated b-string reference into "
                        "list initializer",
                        nullptr);
        }

        ncc_free(*expr_inout);
        *expr_inout = rewritten;
        ncc_free(call_text);
        ncc_free(buffer_expr);
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

static void
validate_container_scan_plan(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                             array_type_info_t *type,
                             array_type_info_t *elem_container,
                             const char *prefix, int count)
{
    array_scan_plan_t plan = {0};
    array_scan_plan_init(&plan, ctx, type, elem_container, prefix, count,
                         false, site);
    array_scan_plan_free(&plan);
}

// Defense-in-depth (WP-018 / WP-016 A1): bound the pow2-ceiling loop so
// a hostile or pathological literal cannot run away. 2^30 entries is far
// beyond any plausible static list literal and matches the cap enforced
// in the n00b static-init helper. Caller passes `site` so the diagnostic
// can be attributed to the literal that overran the limit.
#define NCC_STATIC_CONTAINER_MAX_LEN ((int)1 << 30)

static int
list_static_capacity(ncc_parse_tree_t *site, int len)
{
    if (len < 0 || len > NCC_STATIC_CONTAINER_MAX_LEN) {
        array_errorf(site,
                     "static list literal has too many entries (%d); the "
                     "maximum supported is 2^30",
                     len);
        // array_errorf does not return.
    }
    int cap = 16;
    while (cap < len) {
        cap <<= 1;
    }
    return cap;
}

static char *
build_list_helper_request(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                          list_type_info_t *type, const char *prefix,
                          bool readonly, bool pointer_target,
                          string_list_t *exprs)
{
    int cap = list_static_capacity(site, exprs->count);

    char len_str[32];
    char cap_str[32];
    snprintf(len_str, sizeof(len_str), "%d", exprs->count);
    snprintf(cap_str, sizeof(cap_str), "%d", cap);

    char *type_hex = hex_string(type->object_type, strlen(type->object_type));
    char *elem_hex = hex_string(type->elem_type, strlen(type->elem_type));
    char *type_hash = ncc_static_object_typehash_expr(type->object_type);
    char *elem_hash = ncc_static_object_typehash_expr(type->elem_type);
    char *elem_ptr_type = data_pointer_type_name(type->elem_type);
    char *data_type_hash = ncc_static_object_typehash_expr(elem_ptr_type);
    ncc_free(elem_ptr_type);

    const char *entry_attr = ncc_static_object_entry_attr(ctx);
    char *entry_attr_hex = hex_string(entry_attr, strlen(entry_attr));
    char *identity_namespace =
        ncc_static_object_identity_namespace(ctx, site);
    char *identity_object_key =
        ncc_static_object_identity_key(ctx, "ncc-static-list-object",
                                       site, type_hash, len_str);
    char *identity_payload_key =
        ncc_static_object_identity_key(ctx, "ncc-static-list-data",
                                       site, elem_hash, cap_str);
    char *identity_namespace_hex =
        hex_string(identity_namespace, strlen(identity_namespace));
    char *identity_object_key_hex =
        hex_string(identity_object_key, strlen(identity_object_key));
    char *identity_payload_key_hex =
        hex_string(identity_payload_key, strlen(identity_payload_key));

    array_type_info_t *elem_array = lookup_array_type(ctx, type->elem_type);
    list_type_info_t  *elem_list  = lookup_list_type(ctx, type->elem_type);
    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, (array_type_info_t *)type,
                         elem_array ? elem_array : (array_type_info_t *)elem_list,
                         prefix, cap, true, site);

    char *scan_cb_hex = hex_string(scan_plan.scan_cb,
                                   strlen(scan_plan.scan_cb));
    char *scan_user_hex = hex_string(scan_plan.scan_user,
                                     strlen(scan_plan.scan_user));
    char *shape_decl_hex = hex_string(scan_plan.shape_decl,
                                      strlen(scan_plan.shape_decl));

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "NCC_STATIC_INIT 1\n"
                      "container_kind list\n"
                      "container_target %s\n"
                      "type_hex %s\n"
                      "type_hash %s\n"
                      "element_type_hex %s\n"
                      "element_type_hash %s\n"
                      "data_type_hash %s\n"
                      "prefix %s\n"
                      "readonly %u\n"
                      "len %s\n"
                      "cap %s\n"
                      "abi %zu %zu 8 %s\n"
                      "entry_attr_hex %s\n"
                      "identity_namespace_hex %s\n"
                      "identity_object_key_hex %s\n"
                      "identity_payload_key_hex %s\n"
                      "element_scan_kind %s\n"
                      "element_scan_cb_hex %s\n"
                      "element_scan_user_hex %s\n"
                      "element_shape_decl_hex %s\n"
                      "arg_count %d\n",
                      pointer_target ? "pointer" : "value",
                      type_hex, type_hash, elem_hex, elem_hash,
                      data_type_hash, prefix, readonly ? 1u : 0u,
                      len_str, cap_str, sizeof(void *), sizeof(size_t),
                      static_image_host_endian_value(), entry_attr_hex,
                      identity_namespace_hex, identity_object_key_hex,
                      identity_payload_key_hex, scan_plan.scan_kind,
                      scan_cb_hex, scan_user_hex, shape_decl_hex,
                      exprs->count);

    for (int i = 0; i < exprs->count; i++) {
        char *expr_hex = hex_string(exprs->items[i], strlen(exprs->items[i]));
        ncc_buffer_printf(buf, "arg elem cinit %zu %s\n",
                          strlen(exprs->items[i]), expr_hex);
        ncc_free(expr_hex);
    }

    ncc_buffer_puts(buf, "end\n");

    ncc_free(type_hex);
    ncc_free(elem_hex);
    ncc_free(type_hash);
    ncc_free(elem_hash);
    ncc_free(data_type_hash);
    ncc_free(entry_attr_hex);
    ncc_free(identity_namespace);
    ncc_free(identity_object_key);
    ncc_free(identity_payload_key);
    ncc_free(identity_namespace_hex);
    ncc_free(identity_object_key_hex);
    ncc_free(identity_payload_key_hex);
    ncc_free(scan_cb_hex);
    ncc_free(scan_user_hex);
    ncc_free(shape_decl_hex);
    array_scan_plan_free(&scan_plan);

    return ncc_buffer_take(buf);
}

static char *
build_array_helper_request(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                           array_type_info_t *type,
                           array_type_info_t *elem_array,
                           const char *prefix, string_list_t *exprs)
{
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d", exprs->count);

    char *type_hex = hex_string(type->object_type, strlen(type->object_type));
    char *elem_hex = hex_string(type->elem_type, strlen(type->elem_type));
    char *type_hash = ncc_static_object_typehash_expr(type->object_type);
    char *elem_hash = ncc_static_object_typehash_expr(type->elem_type);
    char *elem_ptr_type = data_pointer_type_name(type->elem_type);
    char *data_type_hash = ncc_static_object_typehash_expr(elem_ptr_type);
    ncc_free(elem_ptr_type);

    const char *entry_attr = ncc_static_object_entry_attr(ctx);
    char *entry_attr_hex = hex_string(entry_attr, strlen(entry_attr));
    char *identity_namespace =
        ncc_static_object_identity_namespace(ctx, site);
    char *identity_object_key =
        ncc_static_object_identity_key(ctx, "ncc-static-array-object",
                                       site, type_hash, count_str);
    char *identity_payload_key =
        ncc_static_object_identity_key(ctx, "ncc-array-data",
                                       site, data_type_hash, count_str);
    char *identity_namespace_hex =
        hex_string(identity_namespace, strlen(identity_namespace));
    char *identity_object_key_hex =
        hex_string(identity_object_key, strlen(identity_object_key));
    char *identity_payload_key_hex =
        hex_string(identity_payload_key, strlen(identity_payload_key));

    array_scan_plan_t scan_plan = {0};
    array_scan_plan_init(&scan_plan, ctx, type, elem_array, prefix,
                         exprs->count, true, site);

    char *scan_cb_hex = hex_string(scan_plan.scan_cb,
                                   strlen(scan_plan.scan_cb));
    char *scan_user_hex = hex_string(scan_plan.scan_user,
                                     strlen(scan_plan.scan_user));
    char *shape_decl_hex = hex_string(scan_plan.shape_decl,
                                      strlen(scan_plan.shape_decl));

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "NCC_STATIC_INIT 1\n"
                      "container_kind array\n"
                      "container_target data\n"
                      "type_hex %s\n"
                      "type_hash %s\n"
                      "element_type_hex %s\n"
                      "element_type_hash %s\n"
                      "data_type_hash %s\n"
                      "prefix %s\n"
                      "readonly 0\n"
                      "len %s\n"
                      "cap %s\n"
                      "abi %zu %zu 8 %s\n"
                      "entry_attr_hex %s\n"
                      "identity_namespace_hex %s\n"
                      "identity_object_key_hex %s\n"
                      "identity_payload_key_hex %s\n"
                      "element_scan_kind %s\n"
                      "element_scan_cb_hex %s\n"
                      "element_scan_user_hex %s\n"
                      "element_shape_decl_hex %s\n"
                      "arg_count %d\n",
                      type_hex, type_hash, elem_hex, elem_hash,
                      data_type_hash, prefix, count_str, count_str,
                      sizeof(void *), sizeof(size_t),
                      static_image_host_endian_value(), entry_attr_hex,
                      identity_namespace_hex, identity_object_key_hex,
                      identity_payload_key_hex, scan_plan.scan_kind,
                      scan_cb_hex, scan_user_hex, shape_decl_hex,
                      exprs->count);

    for (int i = 0; i < exprs->count; i++) {
        char *expr_hex = hex_string(exprs->items[i], strlen(exprs->items[i]));
        ncc_buffer_printf(buf, "arg elem cinit %zu %s\n",
                          strlen(exprs->items[i]), expr_hex);
        ncc_free(expr_hex);
    }

    ncc_buffer_puts(buf, "end\n");

    ncc_free(type_hex);
    ncc_free(elem_hex);
    ncc_free(type_hash);
    ncc_free(elem_hash);
    ncc_free(data_type_hash);
    ncc_free(entry_attr_hex);
    ncc_free(identity_namespace);
    ncc_free(identity_object_key);
    ncc_free(identity_payload_key);
    ncc_free(identity_namespace_hex);
    ncc_free(identity_object_key_hex);
    ncc_free(identity_payload_key_hex);
    ncc_free(scan_cb_hex);
    ncc_free(scan_user_hex);
    ncc_free(shape_decl_hex);
    array_scan_plan_free(&scan_plan);

    return ncc_buffer_take(buf);
}

static char *
lower_array_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                    ncc_parse_tree_t *literal, array_type_info_t *type,
                    string_list_t *decls, bool materialize)
{
    // WP-018 / WP-016 B1: bound nesting depth across array/list/dict
    // recursion. Exits with `array_errorf` past the limit; otherwise
    // pairs with a `_leave` call on every return path.
    ncc_static_container_literal_enter(literal, "array");

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

    char *data_expr = nullptr;
    array_scan_plan_t init_scan_plan = {0};
    char *owned_init_scan_user = nullptr;
    const char *init_scan_kind = "1";
    const char *init_scan_cb = "nullptr";
    const char *init_scan_user = "nullptr";
    if (exprs.count > 0) {
        int id = ctx->unique_id++;
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "__ncc_arraylit_%d", id);

        validate_container_scan_plan(ctx, literal, type, elem_array, prefix,
                                     exprs.count);

        const char *helper = ncc_xform_get_data(ctx)->static_init_helper;
        // Phase 3c.iv: friendly `n00b_array_t(T)` name in user-facing
        // diagnostics, paralleling Phase 3c.iii's dict friendly form,
        // so the missing-helper and helper-failure messages name the
        // type the user wrote instead of leaking the post-mangle
        // `struct ncc_array_<HASH>` symbol.
        char *friendly_type = array_type_friendly_name(type);
        if (!helper || !*helper) {
            list_free(&exprs);
            ncc_array_free(elems);
            array_errorf(literal,
                         "array literal initializer for '%s' requires "
                         "--ncc-static-init-helper=PATH",
                         friendly_type);
            // array_errorf does not return.
        }

        char *request = build_array_helper_request(ctx, literal, type,
                                                   elem_array, prefix, &exprs);
        char *helper_decls = nullptr;
        run_literal_static_init_helper(literal, helper, "array literal",
                                       friendly_type, request,
                                       &data_expr, &helper_decls);
        ncc_free(friendly_type);
        list_push(decls, helper_decls);
        ncc_free(request);

        array_scan_plan_init(&init_scan_plan, ctx, type, elem_array, prefix,
                             exprs.count, false, literal);
        init_scan_kind = init_scan_plan.scan_kind;
        init_scan_cb   = init_scan_plan.scan_cb;
        init_scan_user = init_scan_plan.scan_user;
        if (strcmp(init_scan_kind, "4") == 0
            && strcmp(init_scan_user, "__unused_shape") == 0) {
            ncc_buffer_t *shape_ref = ncc_buffer_empty();
            ncc_buffer_printf(shape_ref, "&%s", init_scan_plan.shape_name);
            owned_init_scan_user = ncc_buffer_take(shape_ref);
            init_scan_user = owned_init_scan_user;
        }
    }

    char *init = build_array_initializer(data_expr, exprs.count,
                                         init_scan_kind, init_scan_cb,
                                         init_scan_user);
    ncc_free(data_expr);
    ncc_free(owned_init_scan_user);
    array_scan_plan_free(&init_scan_plan);
    list_free(&exprs);
    ncc_array_free(elems);

    if (!materialize) {
        ncc_static_container_literal_leave();
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
    ncc_static_container_literal_leave();
    return obj_name;
}

static char *
lower_list_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                   ncc_parse_tree_t *literal, list_type_info_t *type,
                   string_list_t *decls, bool readonly, bool pointer_target)
{
    // WP-018 / WP-016 B1: bound container-literal nesting (see
    // lower_array_literal for the rationale and the shared counter).
    ncc_static_container_literal_enter(literal, "list");

    ncc_array_t(ncc_parse_tree_ptr_t) elems =
        ncc_array_new(ncc_parse_tree_ptr_t, 8);
    collect_array_elements(literal, &elems);

    string_list_t exprs = {0};
    array_type_info_t *elem_array = lookup_array_type(ctx, type->elem_type);
    list_type_info_t  *elem_list  = lookup_list_type(ctx, type->elem_type);
    array_aggregate_type_info_t *elem_aggregate =
        elem_array || elem_list ? nullptr
                                : aggregate_info_from_type_name(ctx,
                                                                type->elem_type);

    if (!elem_array && !elem_list && !elem_aggregate
        && !is_supported_scalar_type(ctx, type->elem_type)) {
        array_errorf(literal,
                     "list literal element type '%s' is not supported for "
                     "static initialization yet; allowed element types are "
                     "scalar numeric/enums, static pointers, r-string "
                     "pointers, compatible b\"...\" buffer pointers, "
                     "aggregate types with static layout, and nested "
                     "ncc/n00b arrays or n00b lists",
                     type->elem_type);
    }

    if (elem_aggregate) {
        validate_static_aggregate_layout(ctx, literal, type->elem_type,
                                         elem_aggregate->specifier, "", 0);
    }

    for (size_t i = 0; i < elems.len; i++) {
        ncc_parse_tree_t *init = elems.data[i];
        ncc_parse_tree_t *nested_array = initializer_array_literal(init);
        ncc_parse_tree_t *nested_list  = initializer_list_literal(init);

        if (elem_array) {
            if (!nested_array) {
                array_errorf(init,
                             "nested array element for list element type "
                             "'%s' must be an array literal like 'a{...}' "
                             "or '[...]'",
                             type->elem_type);
            }
            char *expr = lower_array_literal(ctx, decl, nested_array,
                                             elem_array, decls, false);
            list_push(&exprs, expr);
            continue;
        }

        if (elem_list) {
            if (!nested_list) {
                array_errorf(init,
                             "nested list element for list element type '%s' "
                             "must be a list literal like 'l{...}'",
                             type->elem_type);
            }
            char *expr = lower_list_literal(ctx, decl, nested_list, elem_list,
                                            decls, readonly, false);
            list_push(&exprs, expr);
            continue;
        }

        if (nested_array) {
            array_errorf(init,
                         "list element type '%s' is scalar, so nested array "
                         "literal is not allowed here",
                         type->elem_type);
        }
        if (nested_list) {
            array_errorf(init,
                         "list element type '%s' is not a n00b_list_t(T) "
                         "value, so nested list literal is not allowed here",
                         type->elem_type);
        }

        char *expr = node_text(init);
        if (strstr(expr, "__ncc_rstr")) {
            if (!elem_aggregate && !is_rstr_element_type(ctx,
                                                         type->elem_type)) {
                array_errorf(init,
                             "r-string list element requires element type "
                             "'%s' or a configured r-string pointer type; "
                             "found '%s'",
                             ncc_xform_get_data(ctx)->rstr_string_type,
                             type->elem_type);
            }
            (void)lower_rstrings_in_initializer_expr(ctx, init, decls,
                                                     &expr);
        }
        if (strstr(expr, "__ncc_buflit")) {
            if (!elem_aggregate
                && !is_buffer_pointer_element_type(type->elem_type)) {
                array_errorf(init,
                             "b-string list element requires element type "
                             "'n00b_buffer_t *' or a configured buffer "
                             "pointer typedef; found '%s'",
                             type->elem_type);
            }
            (void)lower_buffers_in_initializer_expr(ctx, init, decls,
                                                    &expr);
        }
        list_push(&exprs, expr);
    }

    int id = ctx->unique_id++;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__ncc_listlit_%d", id);

    validate_container_scan_plan(ctx, literal, (array_type_info_t *)type,
                                 elem_array
                                     ? elem_array
                                     : (array_type_info_t *)elem_list,
                                 prefix,
                                 list_static_capacity(literal, exprs.count));

    const char *helper = ncc_xform_get_data(ctx)->static_init_helper;
    // Phase 3c.iv: friendly `n00b_list_t(T)` name in user-facing
    // diagnostics, paralleling Phase 3c.iii's dict friendly form,
    // so the missing-helper and helper-failure messages name the
    // type the user wrote instead of leaking the post-mangle
    // `struct n00b_list_<HASH>` symbol.
    char *friendly_type = list_type_friendly_name(type);
    if (!helper || !*helper) {
        list_free(&exprs);
        ncc_array_free(elems);
        array_errorf(literal,
                     "list literal initializer for '%s' requires "
                     "--ncc-static-init-helper=PATH",
                     friendly_type);
        // array_errorf does not return.
    }

    char *request = build_list_helper_request(ctx, literal, type, prefix,
                                              readonly, pointer_target,
                                              &exprs);
    char *expr_name = nullptr;
    char *helper_decls = nullptr;
    run_literal_static_init_helper(literal, helper, "list literal",
                                   friendly_type, request,
                                   &expr_name, &helper_decls);
    ncc_free(friendly_type);
    list_push(decls, helper_decls);

    ncc_free(request);
    list_free(&exprs);
    ncc_array_free(elems);

    ncc_static_container_literal_leave();
    return expr_name;
}

// WP-011 Phase 3c.ii.b partial-stub: pointer-keyed dict literals
// whose key type is neither n00b_string_t (r-string) nor
// n00b_buffer_t (buffer) — e.g. typedef'd struct pointers — still
// route to this stub.  Phase 3c.ii.a shipped r-string-key support;
// Phase 3c.ii.b added b-buffer keys; a future phase will widen the
// pointer-key surface to other registered pointer types.  Scalar /
// enum-keyed, r-string-keyed, and buffer-keyed lowering are all
// implemented inline below.
//
// Phase 3c.iii: the diagnostic now names the dict via its friendly
// `n00b_dict_t(K, V)` form (synthesized from `dict_type_info_t`) so
// the user sees the type they wrote, not the post-typeid-mangle
// `struct n00b_dict_<HASH>` symbol.
static void
lower_dict_literal_pointer_key_stub(ncc_parse_tree_t *literal,
                                    const dict_type_info_t *type)
{
    char *friendly = dict_type_friendly_name(type);
    literal_helper_error(literal, "dict literal",
                         friendly,
                         "dict pointer-key lowering not yet implemented "
                         "for this pointer type in WP-011 Phase 3c.ii.b; "
                         "r-string and buffer keys are supported",
                         nullptr, 0);
    ncc_free(friendly);
}

// WP-011 Phase 3c.i: compile-time size of a scalar/enum key type.
// `n00b_hash_raw(key_ptr, ksz)` hashes exactly `ksz` raw bytes of the
// stored key, so ncc must precompute the hash over the same byte
// count that the runtime dict store will use as `sizeof(keys[0])`.
// Enums match the host's `int` width per the C ABI (`-fshort-enums`
// is not used).  This table parallels `is_supported_scalar_type` but
// returns the concrete byte count (0 for unsupported types).
static size_t
scalar_type_size_bytes(const char *type)
{
    if (!type || !*type) {
        return 0;
    }

    struct {
        const char *name;
        size_t      size;
    } table[] = {
        {"bool",                       sizeof(bool)},
        {"char",                       sizeof(char)},
        {"signed char",                sizeof(signed char)},
        {"unsigned char",              sizeof(unsigned char)},
        {"short",                      sizeof(short)},
        {"short int",                  sizeof(short int)},
        {"signed short",               sizeof(signed short)},
        {"signed short int",           sizeof(signed short int)},
        {"unsigned short",             sizeof(unsigned short)},
        {"unsigned short int",         sizeof(unsigned short int)},
        {"int",                        sizeof(int)},
        {"signed",                     sizeof(signed)},
        {"signed int",                 sizeof(signed int)},
        {"unsigned",                   sizeof(unsigned)},
        {"unsigned int",               sizeof(unsigned int)},
        {"long",                       sizeof(long)},
        {"long int",                   sizeof(long int)},
        {"signed long",                sizeof(signed long)},
        {"signed long int",            sizeof(signed long int)},
        {"unsigned long",              sizeof(unsigned long)},
        {"unsigned long int",          sizeof(unsigned long int)},
        {"long long",                  sizeof(long long)},
        {"long long int",              sizeof(long long int)},
        {"unsigned long long",         sizeof(unsigned long long)},
        {"unsigned long long int",     sizeof(unsigned long long int)},
        {"size_t",                     sizeof(size_t)},
        {"int8_t",                     sizeof(int8_t)},
        {"int16_t",                    sizeof(int16_t)},
        {"int32_t",                    sizeof(int32_t)},
        {"int64_t",                    sizeof(int64_t)},
        {"uint8_t",                    sizeof(uint8_t)},
        {"uint16_t",                   sizeof(uint16_t)},
        {"uint32_t",                   sizeof(uint32_t)},
        {"uint64_t",                   sizeof(uint64_t)},
        {"uintptr_t",                  sizeof(uintptr_t)},
        {"intptr_t",                   sizeof(intptr_t)},
        {nullptr,                      0},
    };

    for (int i = 0; table[i].name; i++) {
        if (strcmp(type, table[i].name) == 0) {
            return table[i].size;
        }
    }

    if (strncmp(type, "enum ", 5) == 0) {
        return sizeof(int);
    }

    return 0;
}

// WP-011 Phase 3c.i: classification of a dict key for hashing.  Used
// to gate the scalar-only lowering path; pointer-typed keys route to
// the partial-stub diagnostic so Phase 3c.ii can ship r-string/buffer
// support without disturbing the scalar path.
//
// Phase 3c.ii.a adds DICT_KEY_KIND_RSTRING as a first-class kind so the
// lowering pass can take the r-string-specific path (XXH3 over UTF-8
// content, splice a static rstr ref as the key expression) instead of
// the generic pointer stub.  Buffer keys stay routed to the stub until
// Phase 3c.ii.b lands.
typedef enum {
    DICT_KEY_KIND_SCALAR,   // ints, enums — hash via XXH3 over raw bytes.
    DICT_KEY_KIND_RSTRING,  // r"..." — hash via XXH3 over UTF-8 content.
    DICT_KEY_KIND_BUFFER,   // b"..." — hash via XXH3 over raw bytes.
    DICT_KEY_KIND_POINTER,  // other pointer types — partial-stub.
    DICT_KEY_KIND_UNSUPPORTED,
} dict_key_kind_t;

static dict_key_kind_t
classify_dict_key_type(ncc_xform_ctx_t *ctx, const char *type)
{
    if (!type || !*type) {
        return DICT_KEY_KIND_UNSUPPORTED;
    }
    if (is_rstr_element_type(ctx, type)) {
        return DICT_KEY_KIND_RSTRING;
    }
    if (is_buffer_pointer_element_type(type)) {
        return DICT_KEY_KIND_BUFFER;
    }
    if (is_static_pointer_type(ctx, type)) {
        return DICT_KEY_KIND_POINTER;
    }
    if (scalar_type_size_bytes(type) > 0) {
        return DICT_KEY_KIND_SCALAR;
    }
    return DICT_KEY_KIND_UNSUPPORTED;
}

// WP-011 Phase 3c.i: parse a C integer literal (with optional sign and
// suffix) into the byte representation the runtime would store in the
// `keys[]` slot for a scalar/enum-keyed dict.  This is the input to
// XXH3_128bits to mirror `n00b_hash_raw(key_ptr, ksz)`.  Returns true
// on success.  Hex (`0x...`), octal (leading `0`), and decimal are
// recognised; `'c'` character literals are mapped to a 1-byte value.
// Cast prefixes like `(uint64_t)123` are tolerated by skipping any
// leading parenthesized type name.
static bool
parse_scalar_key_to_bytes(const char *expr, size_t ksz,
                          unsigned char out[16])
{
    if (!expr || ksz == 0 || ksz > 8) {
        return false;
    }

    const char *p = expr;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    // Skip a leading C-style cast like `(int)`.
    if (*p == '(') {
        const char *close = strchr(p, ')');
        if (!close) {
            return false;
        }
        p = close + 1;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
    }

    bool negative = false;
    if (*p == '+' || *p == '-') {
        negative = (*p == '-');
        p++;
    }

    // Character literal: `'c'`.
    if (*p == '\'') {
        const char *q = p + 1;
        int         value = 0;
        if (*q == '\\') {
            q++;
            switch (*q) {
            case 'n': value = '\n'; break;
            case 't': value = '\t'; break;
            case 'r': value = '\r'; break;
            case '0': value = '\0'; break;
            case '\\': value = '\\'; break;
            case '\'': value = '\''; break;
            case '"': value = '"';  break;
            default: return false;
            }
            q++;
        }
        else if (*q && *q != '\'') {
            value = (unsigned char)*q;
            q++;
        }
        else {
            return false;
        }
        if (*q != '\'') {
            return false;
        }
        uint64_t magnitude = (uint64_t)value;
        if (negative) {
            magnitude = (uint64_t)-(int64_t)magnitude;
        }
        memset(out, 0, 16);
        memcpy(out, &magnitude, ksz);
        return true;
    }

    if (!isdigit((unsigned char)*p)) {
        return false;
    }

    int base = 10;
    if (*p == '0') {
        if (p[1] == 'x' || p[1] == 'X') {
            base = 16;
            p += 2;
        }
        else if (p[1] && isdigit((unsigned char)p[1])) {
            base = 8;
            p += 1;
        }
    }

    uint64_t value = 0;
    bool     consumed = false;
    while (*p) {
        unsigned char c   = (unsigned char)*p;
        unsigned int  dig = 0;
        if (c >= '0' && c <= '9') {
            dig = c - '0';
        }
        else if (base == 16 && c >= 'a' && c <= 'f') {
            dig = 10 + (c - 'a');
        }
        else if (base == 16 && c >= 'A' && c <= 'F') {
            dig = 10 + (c - 'A');
        }
        else {
            break;
        }
        if (dig >= (unsigned int)base) {
            return false;
        }
        value = value * (uint64_t)base + dig;
        consumed = true;
        p++;
    }
    if (!consumed) {
        return false;
    }

    // Skip integer suffixes (u, l, ll, ul, ull, uz, etc.) — case-
    // insensitive, just consume contiguous u/l/z chars.
    while (*p && (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L'
                  || *p == 'z' || *p == 'Z')) {
        p++;
    }
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p) {
        return false;
    }

    if (negative) {
        value = (uint64_t)-(int64_t)value;
    }

    memset(out, 0, 16);
    memcpy(out, &value, ksz);
    return true;
}

// WP-011 Phase 3c.i: compute the precomputed scalar-key hash that the
// helper carries through to the bucket's `hv` slot.  Uses
// XXH3_128bits over `ksz` bytes of the parsed key value, matching the
// runtime's `n00b_hash_raw(key_ptr, ksz)` path used by
// `compute_hash()` when `skip_obj_hash = true` and `fn == nullptr`.
//
// MUST NOT be replaced by `n00b_hash_word` or any other primitive —
// the runtime branch this mirrors hashes the raw stored key bytes,
// not the integer value cast to a pointer-sized word, so any other
// hashing produces a mismatched key lookup at runtime.
static void
compute_scalar_key_hash(const unsigned char key_bytes[16], size_t ksz,
                        uint64_t *hash_lo, uint64_t *hash_hi)
{
    XXH128_hash_t h = XXH3_128bits(key_bytes, ksz);
    *hash_lo = (uint64_t)h.low64;
    *hash_hi = (uint64_t)h.high64;
}

// WP-011 Phase 3c.ii.b: compute the precomputed buffer-key hash that
// the helper carries through to the bucket's `hv` slot AND that the
// buffer-literal's descriptor caches at build time (D-066 perf path).
// Mirrors `n00b_buffer_hash` in libn00b's `src/core/hash.c:145-153`:
//
//   if (!b || !b->byte_len) return n00b_hash_word(0ULL);
//   return n00b_xxh_convert(XXH3_128bits(b->data, b->byte_len));
//
// Conversion is the same little-endian {low64,high64} → {hash_lo,hash_hi}
// shape that `compute_string_key_hash` and `compute_scalar_key_hash`
// use.  Empty buffer keys are rejected up-stream (they would map to
// `n00b_hash_word(0ULL)` and the runtime fallback isn't reproducible
// at build time without depending on libn00b's `n00b_word_t` layout).
static void
compute_buffer_key_hash(const unsigned char *bytes, size_t len,
                        uint64_t *out_lo, uint64_t *out_hi)
{
    if (!bytes || len == 0) {
        *out_lo = 0;
        *out_hi = 0;
        return;
    }
    XXH128_hash_t h = XXH3_128bits(bytes, len);
    *out_lo = (uint64_t)h.low64;
    *out_hi = (uint64_t)h.high64;
}

// WP-011 Phase 3c.ii.a: compute the precomputed r-string-key hash that
// the helper carries through to the bucket's `hv` slot.  Mirrors
// `n00b_string_hash` in libn00b's `src/core/hash.c`:
//
//   if (!s || !s->u8_bytes || !s->data) return n00b_hash_word(0ULL);
//   return n00b_xxh_convert(XXH3_128bits(s->data, s->u8_bytes));
//
// Conversion: XXH128_hash_t has `{.low64, .high64}`; `n00b_xxh_convert`
// reinterprets the struct as a 128-bit integer.  On any little-endian
// host (the only supported target) the low 64 bits map to `low64` and
// the high 64 bits map to `high64`, so we copy them across directly,
// the same way `compute_scalar_key_hash` does.
//
// MUST NOT be replaced by any other primitive — the runtime branch
// this mirrors hashes the post-rich-markup UTF-8 bytes, which is what
// `ncc_rstr_build_static_ref` exposes through its `content` /
// `content_len` pair so callers in the dict-key path can feed exactly
// those bytes into XXH3 without re-parsing the r-string literal.
//
// Empty-input fallback: when `u8_len == 0` (or `u8_bytes == nullptr`),
// `n00b_string_hash` returns `n00b_hash_word(0ULL)` — a value this
// helper cannot replicate without depending on libn00b's `n00b_word_t`
// layout.  The lowering pass rejects empty r-string keys ahead of
// time, so this branch returns `(0, 0)` strictly as a placeholder.
static void
compute_string_key_hash(const char *u8_bytes, size_t u8_len,
                        uint64_t *out_lo, uint64_t *out_hi)
{
    if (!u8_bytes || u8_len == 0) {
        *out_lo = 0;
        *out_hi = 0;
        return;
    }
    XXH128_hash_t h = XXH3_128bits(u8_bytes, u8_len);
    *out_lo = (uint64_t)h.low64;
    *out_hi = (uint64_t)h.high64;
}

typedef struct {
    char *key_expr;
    char *value_expr;
    uint64_t hash_lo;
    uint64_t hash_hi;
    uint32_t line;
    uint32_t col;
} dict_pair_t;

static void
dict_pairs_free(dict_pair_t *pairs, size_t count)
{
    if (!pairs) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        ncc_free(pairs[i].key_expr);
        ncc_free(pairs[i].value_expr);
    }
    ncc_free(pairs);
}

// WP-011 Phase 3c.i: helper request builder for scalar-keyed dicts.
// Mirrors `build_list_helper_request`/`build_array_helper_request`
// shape: produces an `NCC_STATIC_INIT 1` request stream with
// container_kind dict, key/value type metadata, scan policy for both
// arrays, per-pair `arg pair cinit` records with precomputed
// `hash <lo> <hi>` modifiers, and identity keys for marshal.
// Pointer-target dicts pass `container_target pointer`; value-target
// dicts pass `container_target value`.
static char *
build_dict_helper_request(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                          dict_type_info_t *type, const char *prefix,
                          bool readonly, bool pointer_target,
                          dict_key_kind_t key_kind,
                          dict_pair_t *pairs, size_t pair_count)
{
    uint64_t entry_count = (uint64_t)pair_count;
    // Defense-in-depth (WP-018 / WP-016 A1): bound the pow2-ceiling loop
    // so a runaway count cannot infinite-loop (the doubling result drops
    // to zero once it crosses 2^63 and the loop never terminates). 2^30
    // entries matches the n00b static-init helper's cap.
    if (entry_count > ((uint64_t)1 << 30)) {
        array_errorf(site,
                     "static dict literal has too many entries (%llu); the "
                     "maximum supported is 2^30",
                     (unsigned long long)entry_count);
        // array_errorf does not return.
    }
    uint64_t cap = 1;
    while (cap < entry_count) {
        cap <<= 1;
    }
    if (cap < 16) {
        cap = 16;
    }

    char len_str[32];
    char cap_str[32];
    snprintf(len_str, sizeof(len_str), "%llu", (unsigned long long)entry_count);
    snprintf(cap_str, sizeof(cap_str), "%llu", (unsigned long long)cap);

    char *type_hex  = hex_string(type->object_type,
                                 strlen(type->object_type));
    char *key_hex   = hex_string(type->key_type, strlen(type->key_type));
    char *value_hex = hex_string(type->value_type, strlen(type->value_type));

    char *type_hash = ncc_static_object_typehash_expr(type->object_type);
    char *key_hash  = ncc_static_object_typehash_expr(type->key_type);
    char *value_hash =
        ncc_static_object_typehash_expr(type->value_type);
    // `data_type_hash` is the typehash for the type-erased store
    // backing.  It does not really correspond to a user-visible type;
    // we use the dict object's own type hash so it is a stable
    // non-zero scalar (the helper rejects `data_type_hash == 0`).
    char *data_type_hash = ncc_static_object_typehash_expr(type->object_type);

    const char *entry_attr = ncc_static_object_entry_attr(ctx);
    char *entry_attr_hex = hex_string(entry_attr, strlen(entry_attr));
    char *identity_namespace =
        ncc_static_object_identity_namespace(ctx, site);
    char *identity_object_key =
        ncc_static_object_identity_key(ctx, "ncc-static-dict-object",
                                       site, type_hash, len_str);
    char *identity_payload_key =
        ncc_static_object_identity_key(ctx, "ncc-static-dict-store",
                                       site, data_type_hash, cap_str);
    char *identity_namespace_hex =
        hex_string(identity_namespace, strlen(identity_namespace));
    char *identity_object_key_hex =
        hex_string(identity_object_key, strlen(identity_object_key));
    char *identity_payload_key_hex =
        hex_string(identity_payload_key, strlen(identity_payload_key));

    // Scan plans for keys[] and values[] arrays.  Scalar-keyed dicts
    // always have POD keys → N00B_GC_SCAN_KIND_NONE.  Pointer-keyed
    // dicts (r-string keys in Phase 3c.ii.a, buffer keys in 3c.ii.b)
    // need N00B_GC_SCAN_KIND_ALL so the GC walks the keys[] array of
    // n00b_string_t / n00b_buffer_t pointers.  Value arrays may hold
    // pointer-bearing aggregates (nested lists, arrays); reuse the
    // array_scan_plan machinery by treating the dict as a synthetic
    // array_type_info_t whose elem_type is the value type.
    bool  key_is_pointer = (key_kind == DICT_KEY_KIND_RSTRING)
                           || (key_kind == DICT_KEY_KIND_BUFFER);
    char *key_scan_kind  = copy_cstr(key_is_pointer ? "2" : "1");
    char *key_scan_cb    = copy_cstr("nullptr");
    char *key_scan_user  = copy_cstr("nullptr");
    char *key_shape_decl = copy_cstr("");

    array_type_info_t value_synth = {
        .object_type = type->object_type,
        .elem_type   = type->value_type,
    };
    array_type_info_t *value_elem_array =
        lookup_array_type(ctx, type->value_type);
    list_type_info_t  *value_elem_list  =
        lookup_list_type(ctx, type->value_type);
    array_scan_plan_t value_scan_plan = {0};
    char value_shape_prefix[80];
    snprintf(value_shape_prefix, sizeof(value_shape_prefix),
             "%s_values", prefix);
    array_scan_plan_init(&value_scan_plan, ctx, &value_synth,
                         value_elem_array
                             ? value_elem_array
                             : (array_type_info_t *)value_elem_list,
                         value_shape_prefix, (int)cap, true, site);

    char *value_scan_kind = copy_cstr(value_scan_plan.scan_kind);
    char *value_scan_cb   = copy_cstr(value_scan_plan.scan_cb);
    char *value_scan_user = copy_cstr(value_scan_plan.scan_user);
    char *value_shape_decl = copy_cstr(value_scan_plan.shape_decl
                                           ? value_scan_plan.shape_decl
                                           : "");

    char *key_scan_cb_hex   = hex_string(key_scan_cb, strlen(key_scan_cb));
    char *key_scan_user_hex = hex_string(key_scan_user,
                                         strlen(key_scan_user));
    char *key_shape_decl_hex = hex_string(key_shape_decl,
                                          strlen(key_shape_decl));
    char *value_scan_cb_hex = hex_string(value_scan_cb,
                                         strlen(value_scan_cb));
    char *value_scan_user_hex = hex_string(value_scan_user,
                                           strlen(value_scan_user));
    char *value_shape_decl_hex = hex_string(value_shape_decl,
                                            strlen(value_shape_decl));

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "NCC_STATIC_INIT 1\n"
                      "container_kind dict\n"
                      "container_target %s\n"
                      "type_hex %s\n"
                      "type_hash %s\n"
                      "key_type_hex %s\n"
                      "key_type_hash %s\n"
                      "value_type_hex %s\n"
                      "value_type_hash %s\n"
                      "data_type_hash %s\n"
                      "prefix %s\n"
                      "readonly %u\n"
                      "len %s\n"
                      "cap %s\n"
                      "abi %zu %zu 8 %s\n"
                      "entry_attr_hex %s\n"
                      "identity_namespace_hex %s\n"
                      "identity_object_key_hex %s\n"
                      "identity_payload_key_hex %s\n"
                      "key_scan_kind %s\n"
                      "key_scan_cb_hex %s\n"
                      "key_scan_user_hex %s\n"
                      "key_shape_decl_hex %s\n"
                      "value_scan_kind %s\n"
                      "value_scan_cb_hex %s\n"
                      "value_scan_user_hex %s\n"
                      "value_shape_decl_hex %s\n"
                      "skip_obj_hash %u\n"
                      "cached_hash_emit %s\n"
                      "arg_count %zu\n",
                      pointer_target ? "pointer" : "value",
                      type_hex, type_hash, key_hex, key_hash, value_hex,
                      value_hash, data_type_hash, prefix,
                      readonly ? 1u : 0u, len_str, cap_str,
                      sizeof(void *), sizeof(size_t),
                      static_image_host_endian_value(), entry_attr_hex,
                      identity_namespace_hex, identity_object_key_hex,
                      identity_payload_key_hex,
                      key_scan_kind, key_scan_cb_hex, key_scan_user_hex,
                      key_shape_decl_hex,
                      value_scan_kind, value_scan_cb_hex,
                      value_scan_user_hex, value_shape_decl_hex,
                      // WP-011 Phase 3c.ii.a/3c.ii.b: pointer-keyed
                      // dicts (r-string and buffer) call `n00b_hash()`
                      // on the key pointer's target at runtime
                      // (skip_obj_hash=0), but the descriptor's
                      // cached_hash slot lets that call short-circuit;
                      // ncc emits the cached_hash via the helper's
                      // bucket `hv` slot, hence cached_hash_emit=1.
                      // For buffer keys ncc additionally threads the
                      // cached_hash into the buffer object descriptor
                      // via `cached_hash_lo`/`cached_hash_hi` args on
                      // the buffer literal request (handled in
                      // `lower_buffer_literal_ref_with_hash`).
                      // Scalar-keyed dicts continue to hash raw key
                      // bytes via XXH3 (skip_obj_hash=1).
                      key_is_pointer ? 0u : 1u,
                      // WP-011 Phase 5a: the helper expects 'yes'/'no'
                      // here (matches its other boolean fields).  Pre-
                      // Phase 5a no existing fixture exercised this
                      // path against the strict n00b helper — the
                      // n00b-side rstr matcher rejected `n00b_string_t *`
                      // so key_is_pointer was false for the canonical
                      // key shape, and the ncc-side test helper silently
                      // ignores this token.  Now that the matcher is
                      // widened (Phase 5a finding 2) we route to the
                      // strict helper for the regression test, so the
                      // wire value must be 'yes'/'no' rather than
                      // '1'/'0'.
                      key_is_pointer ? "yes" : "no",
                      pair_count);

    for (size_t i = 0; i < pair_count; i++) {
        size_t key_len = strlen(pairs[i].key_expr);
        size_t val_len = strlen(pairs[i].value_expr);
        char  *key_expr_hex = hex_string(pairs[i].key_expr, key_len);
        char  *val_expr_hex = hex_string(pairs[i].value_expr, val_len);
        ncc_buffer_printf(buf,
                          "arg - pair cinit %zu %s %zu %s "
                          "hash %llu %llu\n",
                          key_len, key_expr_hex, val_len, val_expr_hex,
                          (unsigned long long)pairs[i].hash_lo,
                          (unsigned long long)pairs[i].hash_hi);
        ncc_free(key_expr_hex);
        ncc_free(val_expr_hex);
    }

    ncc_buffer_puts(buf, "end\n");

    ncc_free(type_hex);
    ncc_free(key_hex);
    ncc_free(value_hex);
    ncc_free(type_hash);
    ncc_free(key_hash);
    ncc_free(value_hash);
    ncc_free(data_type_hash);
    ncc_free(entry_attr_hex);
    ncc_free(identity_namespace);
    ncc_free(identity_object_key);
    ncc_free(identity_payload_key);
    ncc_free(identity_namespace_hex);
    ncc_free(identity_object_key_hex);
    ncc_free(identity_payload_key_hex);
    ncc_free(key_scan_kind);
    ncc_free(key_scan_cb);
    ncc_free(key_scan_user);
    ncc_free(key_shape_decl);
    ncc_free(key_scan_cb_hex);
    ncc_free(key_scan_user_hex);
    ncc_free(key_shape_decl_hex);
    ncc_free(value_scan_kind);
    ncc_free(value_scan_cb);
    ncc_free(value_scan_user);
    ncc_free(value_shape_decl);
    ncc_free(value_scan_cb_hex);
    ncc_free(value_scan_user_hex);
    ncc_free(value_shape_decl_hex);
    array_scan_plan_free(&value_scan_plan);

    return ncc_buffer_take(buf);
}

// WP-011 Phase 3c.i+3c.ii.a: scalar/enum and r-string-keyed dict
// literal lowering.  Inspects each key's classified type: scalar/enum
// keys are precomputed via XXH3_128bits over raw key bytes (matching
// `n00b_hash_raw`); r-string keys are precomputed via XXH3_128bits
// over the post-rich-markup UTF-8 content (matching
// `n00b_string_hash`).  Both feed the helper's `hash <lo> <hi>` pair
// modifier so the bucket `hv` field carries the correct value at
// startup.  Buffer-keyed and other pointer-typed dicts route to the
// Phase 3c.ii.a partial-stub diagnostic so Phase 3c.ii.b can ship
// buffer-key hashing without disturbing the existing paths.
// Duplicate keys at compile time (D-065) are rejected here by hash
// comparison so the diagnostic can name BOTH source positions.
static char *
lower_dict_literal(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                   ncc_parse_tree_t *literal, dict_type_info_t *type,
                   string_list_t *decls, bool readonly, bool pointer_target)
{
    (void)decl;

    // WP-018 / WP-016 B1: bound container-literal nesting (see
    // lower_array_literal for the rationale and the shared counter).
    ncc_static_container_literal_enter(literal, "dict");

    // Collect pairs from the dict literal.  Both spellings reach this
    // path (explicit `d{...}` and bare `{k: v, ...}`), with a
    // <dict_pair_list> child carrying <dict_pair> nodes whose first
    // child is the key initializer and last is the value initializer.
    ncc_parse_tree_t *pair_list = first_descendant_nt(literal,
                                                      "dict_pair_list");
    ncc_array_t(ncc_parse_tree_ptr_t) pairs_nodes =
        ncc_array_new(ncc_parse_tree_ptr_t, 8);
    if (pair_list) {
        collect_nt_children(pair_list, "dict_pair", &pairs_nodes);
    }

    dict_key_kind_t  key_kind = DICT_KEY_KIND_SCALAR;
    size_t           ksz      = scalar_type_size_bytes(type->key_type);
    if (ksz == 0) {
        key_kind = classify_dict_key_type(ctx, type->key_type);
    }

    // WP-011 Phase 3c.ii.a: r-string-keyed dicts get full lowering with
    // precomputed XXH3_128bits over UTF-8 content (mirroring
    // `n00b_string_hash`).
    // WP-011 Phase 3c.ii.b: buffer-keyed dicts now also get full
    // lowering, mirroring `n00b_buffer_hash`.  Other pointer-typed
    // keys (typedef'd struct pointers, etc.) still route to the
    // partial-stub diagnostic until a future phase lands a generic
    // pointer-key path.
    if (key_kind == DICT_KEY_KIND_POINTER) {
        ncc_array_free(pairs_nodes);
        lower_dict_literal_pointer_key_stub(literal, type);
        ncc_static_container_literal_leave();
        return nullptr;
    }
    if (key_kind == DICT_KEY_KIND_UNSUPPORTED) {
        ncc_array_free(pairs_nodes);
        array_errorf(literal,
                     "dict literal key type '%s' is not supported for "
                     "static initialization yet; allowed key types are "
                     "scalar integers/enums (Phase 3c.i), r-string keys "
                     "(Phase 3c.ii.a), and buffer keys (Phase 3c.ii.b)",
                     type->key_type);
    }

    // Value-type validation parallels the list-literal precedent.
    array_type_info_t *value_elem_array =
        lookup_array_type(ctx, type->value_type);
    list_type_info_t  *value_elem_list  =
        lookup_list_type(ctx, type->value_type);
    dict_type_info_t  *value_elem_dict  =
        lookup_dict_type(ctx, type->value_type);
    array_aggregate_type_info_t *value_elem_aggregate =
        (value_elem_array || value_elem_list || value_elem_dict)
            ? nullptr
            : aggregate_info_from_type_name(ctx, type->value_type);

    if (!value_elem_array && !value_elem_list && !value_elem_dict
        && !value_elem_aggregate
        && !is_supported_scalar_type(ctx, type->value_type)) {
        ncc_array_free(pairs_nodes);
        array_errorf(literal,
                     "dict literal value type '%s' is not supported for "
                     "static initialization yet; allowed value types are "
                     "scalar numeric/enums, static pointers, r-string "
                     "pointers, compatible b\"...\" buffer pointers, "
                     "aggregate types with static layout, and nested "
                     "ncc/n00b arrays, lists, or dicts",
                     type->value_type);
    }

    // Build the per-pair list with hashes (scalar-key only).
    size_t pair_count = pairs_nodes.len;
    dict_pair_t *pairs = pair_count
                            ? ncc_alloc_array(dict_pair_t, pair_count)
                            : nullptr;

    for (size_t i = 0; i < pair_count; i++) {
        ncc_parse_tree_t *pair_node = pairs_nodes.data[i];
        ncc_array_t(ncc_parse_tree_ptr_t) inits =
            ncc_array_new(ncc_parse_tree_ptr_t, 2);
        collect_nt_children(pair_node, "initializer", &inits);
        if (inits.len < 2) {
            ncc_array_free(inits);
            ncc_array_free(pairs_nodes);
            dict_pairs_free(pairs, i);
            array_errorf(pair_node, "internal: dict pair missing key/value "
                                    "initializer children");
        }
        ncc_parse_tree_t *key_init   = inits.data[0];
        ncc_parse_tree_t *value_init = inits.data[1];
        ncc_array_free(inits);

        uint32_t line = 0;
        uint32_t col  = 0;
        ncc_xform_first_leaf_pos(key_init, &line, &col);

        // Key expression and hash.  For scalar keys we render the
        // textual form and parse the value bytes to feed XXH3.  For
        // r-string keys (Phase 3c.ii.a) we locate the embedded
        // `__ncc_rstr(...)` call, emit a static r-string descriptor
        // via `ncc_rstr_build_static_ref`, push that decl into the
        // generated decls list, and use the static-ref expression
        // (e.g., `&_ncc_rs_N`) as the dict key expression.  The hash
        // matches what `n00b_string_hash` would compute at runtime
        // (`XXH3_128bits(s->data, s->u8_bytes)`), precomputed by
        // `ncc_rstr_build_static_ref` from the post-rich-markup
        // UTF-8 content bytes.
        char    *key_expr = nullptr;
        uint64_t hash_lo  = 0;
        uint64_t hash_hi  = 0;

        if (key_kind == DICT_KEY_KIND_RSTRING) {
            ncc_array_t(ncc_parse_tree_ptr_t) rstr_calls =
                ncc_array_new(ncc_parse_tree_ptr_t, 1);
            collect_rstr_calls(key_init, &rstr_calls);
            if (rstr_calls.len != 1) {
                size_t found = rstr_calls.len;
                ncc_array_free(rstr_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "r-string-keyed dict literal requires each "
                             "key to be a single `r\"...\"` literal "
                             "(found %zu r-string calls in this key); "
                             "computed/concatenated r-string keys are "
                             "not supported in WP-011 Phase 3c.ii.a",
                             found);
            }

            // Pre-flight: build the static ref with cached_hash=0 so
            // we can read the UTF-8 content, then immediately throw
            // that result away and rebuild via the `_ex` entrypoint
            // with the precomputed XXH3 value formatted into the
            // descriptor's `.cached_hash` slot expression (WP-011
            // Phase 3c.ii.b realizes D-066's runtime perf path).
            //
            // The two-pass shape keeps the helper request's bucket
            // `hv` slot and the descriptor's cached_hash slot bit-
            // identical: both come from the same
            // `compute_string_key_hash` invocation on the same
            // post-rich-markup bytes.
            ncc_rstr_static_ref_t rstr_probe =
                ncc_rstr_build_static_ref(ctx, rstr_calls.data[0]);
            if (!rstr_probe.decl || !rstr_probe.expr) {
                ncc_free(rstr_probe.decl);
                ncc_free(rstr_probe.expr);
                ncc_free(rstr_probe.content);
                ncc_array_free(rstr_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "could not lower r-string key for "
                             "dict literal");
            }
            if (!rstr_probe.content || rstr_probe.content_len == 0) {
                ncc_free(rstr_probe.decl);
                ncc_free(rstr_probe.expr);
                ncc_free(rstr_probe.content);
                ncc_array_free(rstr_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "empty r-string key for dict literal is "
                             "not supported (would map to the "
                             "n00b_hash_word(0) fallback path)");
            }

            compute_string_key_hash(rstr_probe.content, rstr_probe.content_len,
                                    &hash_lo, &hash_hi);

            // Drop the probe's outputs; rebuild with the cached_hash
            // expression threaded through.  The probe's `decl`/`expr`
            // would otherwise leak generated decls that the static-
            // ref's `_ex` rebuild renames via a fresh `unique_id`.
            ncc_free(rstr_probe.decl);
            ncc_free(rstr_probe.expr);
            ncc_free(rstr_probe.content);

            char cached_hash_expr[96];
            // The descriptor template casts this expression to
            // `n00b_uint128_t`.  We build `((HI<<64)|LO)` so the
            // resulting integer is bit-identical to the 128-bit
            // value `XXH3_128bits` produced — which is exactly the
            // value the runtime probe loop compares against the
            // bucket `hv` slot.  Using `n00b_uint128_t` casts on
            // both operands keeps the shift safe (no UB from
            // shifting a 64-bit value past its width).
            snprintf(cached_hash_expr, sizeof(cached_hash_expr),
                     "(((n00b_uint128_t)0x%016llxULL << 64)"
                     "|(n00b_uint128_t)0x%016llxULL)",
                     (unsigned long long)hash_hi,
                     (unsigned long long)hash_lo);

            ncc_rstr_static_ref_t rstr =
                ncc_rstr_build_static_ref_ex(ctx, rstr_calls.data[0],
                                             cached_hash_expr);
            ncc_array_free(rstr_calls);
            if (!rstr.decl || !rstr.expr) {
                ncc_free(rstr.decl);
                ncc_free(rstr.expr);
                ncc_free(rstr.content);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "could not lower r-string key for "
                             "dict literal (cached_hash pass)");
            }

            list_push(decls, rstr.decl);
            ncc_free(rstr.content);
            key_expr = rstr.expr;
        }
        else if (key_kind == DICT_KEY_KIND_BUFFER) {
            // WP-011 Phase 3c.ii.b: buffer-keyed dicts mirror the
            // r-string-keyed path.  Each key must be exactly one
            // `b"..."` literal (computed / concatenated buffers are
            // out of scope here, same as r-strings).  ncc decodes
            // the literal's bytes, hashes them with the same
            // `XXH3_128bits` sequence `n00b_buffer_hash` uses at
            // runtime, threads the bucket `hv` slot AND the buffer
            // object descriptor's `.cached_hash` slot through to
            // realise D-066's runtime perf path.
            ncc_array_t(ncc_parse_tree_ptr_t) buf_calls =
                ncc_array_new(ncc_parse_tree_ptr_t, 1);
            collect_buflit_calls(key_init, &buf_calls);
            if (buf_calls.len != 1) {
                size_t found = buf_calls.len;
                ncc_array_free(buf_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "buffer-keyed dict literal requires each "
                             "key to be a single `b\"...\"` literal "
                             "(found %zu buffer literal calls in this "
                             "key); computed/concatenated buffer keys "
                             "are not supported in WP-011 Phase 3c.ii.b",
                             found);
            }

            // First pass: decode bytes via the helper-backed path
            // with cached_hash=0 just so we can recover the bytes
            // (which the prescan stashed into the call subtree);
            // but to keep parity with the rstring two-pass shape,
            // we instead grab the bytes directly via the
            // `out_payload_*` outputs and emit the real request
            // with the cached_hash threaded through.  That is a
            // single helper invocation, not two — the helper-
            // emitted decl already names the buffer object
            // (`_obj`), so we can splice that as the key expression.
            unsigned char *bytes_copy   = nullptr;
            size_t         bytes_len    = 0;

            // Pre-decode the bytes so we know the hash before we
            // invoke the helper.  We then call
            // `lower_buffer_literal_ref_with_hash` with the hash
            // already known; that function decodes the bytes a
            // second time internally, but the cost is tiny and the
            // shape stays simple.
            ncc_buffer_t *probe = ncc_buffer_empty();
            int probe_count = 0;
            if (!decode_buflit_call_strings(buf_calls.data[0], probe,
                                            &probe_count)
                || probe_count == 0) {
                ncc_buffer_free(probe);
                ncc_array_free(buf_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "could not decode buffer key for "
                             "dict literal");
            }
            if (probe->byte_len == 0) {
                ncc_buffer_free(probe);
                ncc_array_free(buf_calls);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "empty buffer key for dict literal is "
                             "not supported (would map to the "
                             "n00b_hash_word(0) fallback path)");
            }

            compute_buffer_key_hash((const unsigned char *)probe->data,
                                    probe->byte_len, &hash_lo, &hash_hi);
            ncc_buffer_free(probe);

            // Second pass: emit the static buffer image with the
            // precomputed cached_hash threaded into the helper's
            // arg list.  The helper returns the buffer object's
            // address as the expression (e.g. `&_ncc_buflit_N_obj`).
            char *buf_expr = lower_buffer_literal_ref_with_hash(
                ctx, buf_calls.data[0], decls,
                hash_lo, hash_hi, &bytes_copy, &bytes_len);
            ncc_array_free(buf_calls);
            // We have the bytes in `bytes_copy` already; the second
            // decode is just used for hash-stability cross-check.
            // No-op out here.
            ncc_free(bytes_copy);
            key_expr = buf_expr;
        }
        else {
            key_expr = node_text(key_init);
            unsigned char key_bytes[16];
            if (!parse_scalar_key_to_bytes(key_expr, ksz, key_bytes)) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(key_init,
                             "dict literal key for type '%s' must be a "
                             "compile-time integer/character literal "
                             "(Phase 3c.i scalar-key lowering); "
                             "non-literal expressions are reserved for "
                             "a future phase",
                             type->key_type);
            }

            compute_scalar_key_hash(key_bytes, ksz, &hash_lo, &hash_hi);
        }

        // Value expression handling: nested array/list/dict literals
        // recurse through their respective lowering paths; r-string
        // and b-buffer references are spliced inline.
        ncc_parse_tree_t *nested_array =
            initializer_array_literal(value_init);
        ncc_parse_tree_t *nested_list  =
            initializer_list_literal(value_init);
        ncc_parse_tree_t *nested_dict  =
            initializer_dict_literal(value_init);
        char *value_expr = nullptr;
        if (value_elem_array) {
            if (!nested_array) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "nested array value for dict value type "
                             "'%s' must be an array literal like "
                             "'a{...}' or '[...]'",
                             type->value_type);
            }
            value_expr = lower_array_literal(ctx, decl, nested_array,
                                             value_elem_array, decls,
                                             false);
        }
        else if (value_elem_list) {
            if (!nested_list) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "nested list value for dict value type '%s' "
                             "must be a list literal like 'l{...}'",
                             type->value_type);
            }
            value_expr = lower_list_literal(ctx, decl, nested_list,
                                            value_elem_list, decls,
                                            readonly, false);
        }
        else if (value_elem_dict) {
            if (!nested_dict) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "nested dict value for dict value type '%s' "
                             "must be a dict literal like 'd{...}' "
                             "or '{k: v, ...}'",
                             type->value_type);
            }
            value_expr = lower_dict_literal(ctx, decl, nested_dict,
                                            value_elem_dict, decls,
                                            readonly, false);
        }
        else {
            if (nested_array) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "dict value type '%s' is scalar, so nested "
                             "array literal is not allowed here",
                             type->value_type);
            }
            if (nested_list) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "dict value type '%s' is not a "
                             "n00b_list_t(T) value, so nested list "
                             "literal is not allowed here",
                             type->value_type);
            }
            if (nested_dict) {
                ncc_free(key_expr);
                ncc_array_free(pairs_nodes);
                dict_pairs_free(pairs, i);
                array_errorf(value_init,
                             "dict value type '%s' is not a "
                             "n00b_dict_t(K, V) value, so nested dict "
                             "literal is not allowed here",
                             type->value_type);
            }

            value_expr = node_text(value_init);
            if (strstr(value_expr, "__ncc_rstr")) {
                if (!value_elem_aggregate
                    && !is_rstr_element_type(ctx, type->value_type)) {
                    ncc_free(key_expr);
                    ncc_free(value_expr);
                    ncc_array_free(pairs_nodes);
                    dict_pairs_free(pairs, i);
                    array_errorf(value_init,
                                 "r-string dict value requires value type "
                                 "'%s' or a configured r-string pointer "
                                 "type; found '%s'",
                                 ncc_xform_get_data(ctx)->rstr_string_type,
                                 type->value_type);
                }
                (void)lower_rstrings_in_initializer_expr(ctx, value_init,
                                                        decls,
                                                        &value_expr);
            }
            if (strstr(value_expr, "__ncc_buflit")) {
                if (!value_elem_aggregate
                    && !is_buffer_pointer_element_type(type->value_type)) {
                    ncc_free(key_expr);
                    ncc_free(value_expr);
                    ncc_array_free(pairs_nodes);
                    dict_pairs_free(pairs, i);
                    array_errorf(value_init,
                                 "b-string dict value requires value type "
                                 "'n00b_buffer_t *' or a configured "
                                 "buffer pointer typedef; found '%s'",
                                 type->value_type);
                }
                (void)lower_buffers_in_initializer_expr(ctx, value_init,
                                                       decls,
                                                       &value_expr);
            }
        }

        // Duplicate-key check (D-065).  We compare the 128-bit hash;
        // collisions in the precomputed scalar hash for distinct keys
        // would also be caught by the helper's slot-assignment pass,
        // but we do it here first so the diagnostic can name both
        // source positions and the duplicated key text.
        for (size_t j = 0; j < i; j++) {
            if (pairs[j].hash_lo == hash_lo && pairs[j].hash_hi == hash_hi) {
                array_errorf(key_init,
                             "duplicate key %s in dict literal "
                             "(first occurrence at line %u col %u, "
                             "this occurrence at line %u col %u)",
                             key_expr, pairs[j].line, pairs[j].col,
                             line, col);
            }
        }

        pairs[i].key_expr   = key_expr;
        pairs[i].value_expr = value_expr;
        pairs[i].hash_lo    = hash_lo;
        pairs[i].hash_hi    = hash_hi;
        pairs[i].line       = line;
        pairs[i].col        = col;
    }

    ncc_array_free(pairs_nodes);

    int id = ctx->unique_id++;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__ncc_dictlit_%d", id);

    const char *helper = ncc_xform_get_data(ctx)->static_init_helper;
    // Phase 3c.iii: friendly `n00b_dict_t(K, V)` name in user-facing
    // diagnostics, used both for the missing-helper error and for any
    // helper-launch error surfaced by `run_literal_static_init_helper`.
    char *friendly_type = dict_type_friendly_name(type);
    if (!helper || !*helper) {
        dict_pairs_free(pairs, pair_count);
        array_errorf(literal,
                     "dict literal initializer for '%s' requires "
                     "--ncc-static-init-helper=PATH",
                     friendly_type);
        // array_errorf does not return.
    }

    char *request = build_dict_helper_request(ctx, literal, type, prefix,
                                              readonly, pointer_target,
                                              key_kind,
                                              pairs, pair_count);
    char *expr_name = nullptr;
    char *helper_decls = nullptr;
    run_literal_static_init_helper(literal, helper, "dict literal",
                                   friendly_type, request,
                                   &expr_name, &helper_decls);
    ncc_free(friendly_type);
    list_push(decls, helper_decls);

    ncc_free(request);
    dict_pairs_free(pairs, pair_count);

    ncc_static_container_literal_leave();
    return expr_name;
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

static void
record_list_typedef_aliases(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                            ncc_parse_tree_t *decl_specs,
                            list_type_info_t *base_info)
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
            record_list_type(ctx, alias, alias, base_info->elem_type);
            ncc_free(alias);
        }
    }

    ncc_array_free(init_decls);
}

// WP-011 Phase 3c: register `typedef n00b_dict_t(K, V) my_dict_t;`
// aliases so the subsequent declaration `my_dict_t x = d{...};` can be
// resolved by typedef-name lookup.  Mirrors
// `record_list_typedef_aliases` for the dict_types table.
static void
record_dict_typedef_aliases(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                            ncc_parse_tree_t *decl_specs,
                            dict_type_info_t *base_info)
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
            record_dict_type(ctx, alias, alias, base_info->key_type,
                             base_info->value_type);
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
    list_type_info_t *list_type = list_type_from_decl_specs(ctx, decl_specs);
    record_list_typedef_aliases(ctx, decl, decl_specs, list_type);
    // WP-011 Phase 3c: full dict K/V resolution with typedef-alias
    // recording, mirroring the list precedent.  Phase 2 only had a
    // boolean dict-target check; the lowering pass needs the resolved
    // K/V types so the helper request fields can be built.
    dict_type_info_t *dict_type = dict_type_from_decl_specs(ctx, decl_specs);
    record_dict_typedef_aliases(ctx, decl, decl_specs, dict_type);
    bool dict_target = dict_type != nullptr;

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
        ncc_parse_tree_t *list_literal = initializer_list_literal(init);
        // WP-011: dict literal detection (explicit `d{...}` and bare
        // `{key: value, ...}`).  Sits before the unknown-modifier
        // diagnostic so the dict path consumes `d{...}` instead of
        // tripping the legacy a/l-only error.
        ncc_parse_tree_t *dict_literal = initializer_dict_literal(init);
        ncc_parse_tree_t *modified = initializer_modified_literal(init);
        if (modified && !literal && !list_literal && !dict_literal) {
            const char *modifier = modified_literal_modifier(modified);
            array_errorf(modified,
                         "unknown literal modifier '%s{...}'; supported "
                         "literal modifiers are 'a{...}' for arrays, "
                         "'l{...}' for lists, and 'd{...}' for dicts",
                         modifier ? modifier : "<unknown>");
        }
        if (dict_literal) {
            ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
                init_decls.data[i], "declarator");
            char *name = declarator_name(declarator);

            // Target-type check: bare `{key: value, ...}` is dict-only
            // (the parser admits `{...}` as a dict literal only when
            // it contains `:` separators; D-063).  An explicit `d{...}`
            // is also dict-only.  Either way, the target type must be
            // a dict.
            if (!dict_target) {
                char *target = node_text(decl_specs);
                array_errorf(dict_literal,
                             "dict literal initializer for '%s' requires a "
                             "n00b_dict_t(K, V) object or pointer target; "
                             "found target type '%s'",
                             name ? name : "<unknown>", target);
            }

            // Lifetime check parallels the list literal rule (WP-010):
            // block-scope mutable dict targets are rejected ahead of
            // lowering, matching the list literal precedent so the
            // diagnostic fires before lowering.
            bool local = ncc_xform_find_ancestor(decl, "block_item") != nullptr;
            bool readonly = decl_is_const(decl_specs);
            if (local && !readonly) {
                array_errorf(dict_literal,
                             "non-const local dict literal for '%s' is not "
                             "supported yet; add 'const' to the pointee/object "
                             "type or move the declaration to file scope",
                             name ? name : "<unknown>");
            }

            // WP-011 Phase 3c.i: full scalar/enum-keyed dict lowering.
            // Pointer-keyed dicts route to the partial-stub diagnostic
            // inside `lower_dict_literal`; that path does not return.
            bool pointer_target =
                pointer_depth_in_declarator(declarator) > 0;
            string_list_t dict_decls = {0};
            char *replacement_src = lower_dict_literal(ctx, decl,
                                                       dict_literal,
                                                       dict_type, &dict_decls,
                                                       readonly,
                                                       pointer_target);

            for (int d = 0; d < dict_decls.count; d++) {
                insert_generated_decl(ctx, decl, dict_decls.items[d]);
            }

            ncc_parse_tree_t *replacement = ncc_xform_parse_source(
                ctx->grammar, "initializer", replacement_src,
                "xform_array_literal");
            if (!replacement) {
                fprintf(stderr, "ncc: error: failed to parse generated dict "
                                "initializer:\n%s\n", replacement_src);
                exit(1);
            }

            replace_initializer(init_decls.data[i], replacement);
            ncc_free(replacement_src);
            ncc_free(name);
            list_free(&dict_decls);
            continue;
        }
        if (list_literal) {
            ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
                init_decls.data[i], "declarator");
            char *name = declarator_name(declarator);

            if (!list_type) {
                char *target = node_text(decl_specs);
                array_errorf(list_literal,
                             "list literal initializer for '%s' requires a "
                             "n00b_list_t(T) object or pointer target; found "
                             "target type '%s'",
                             name ? name : "<unknown>", target);
            }

            bool local = ncc_xform_find_ancestor(decl, "block_item") != nullptr;
            bool readonly = decl_is_const(decl_specs);
            if (local && !readonly) {
                array_errorf(list_literal,
                             "non-const local list literal for '%s' is not "
                             "supported yet; add 'const' to the pointee/object "
                             "type or move the declaration to file scope",
                             name ? name : "<unknown>");
            }

            bool pointer_target =
                pointer_depth_in_declarator(declarator) > 0;
            string_list_t decls = {0};
            char *replacement_src = lower_list_literal(ctx, decl, list_literal,
                                                       list_type, &decls,
                                                       readonly,
                                                       pointer_target);

            for (int d = 0; d < decls.count; d++) {
                insert_generated_decl(ctx, decl, decls.items[d]);
            }

            ncc_parse_tree_t *replacement = ncc_xform_parse_source(
                ctx->grammar, "initializer", replacement_src,
                "xform_array_literal");
            if (!replacement) {
                fprintf(stderr, "ncc: error: failed to parse generated list "
                                "initializer:\n%s\n", replacement_src);
                exit(1);
            }

            replace_initializer(init_decls.data[i], replacement);
            ncc_free(replacement_src);
            ncc_free(name);
            list_free(&decls);
            continue;
        }
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
