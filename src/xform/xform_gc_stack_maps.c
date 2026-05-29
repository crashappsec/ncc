// xform_gc_stack_maps.c -- Discover n00b GC stack roots for later emission.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_type_layout.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
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
format_cstr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);

    if (needed < 0) {
        va_end(ap);
        return copy_cstr("");
    }

    char *result = ncc_alloc_size(1, (size_t)needed + 1);
    vsnprintf(result, (size_t)needed + 1, fmt, ap);
    va_end(ap);
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

static bool
cstr_has_prefix(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

typedef ncc_layout_parse_tree_list_t parse_tree_list_t;
typedef ncc_layout_uint64_list_t uint64_list_t;
typedef ncc_layout_aggregate_type_info_t aggregate_type_info_t;

static void
parse_tree_list_push(parse_tree_list_t *list, ncc_parse_tree_t *node)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(ncc_parse_tree_t *));
        list->cap = new_cap;
    }

    list->data[list->len++] = node;
}

static void
uint64_list_push(uint64_list_t *list, uint64_t value)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 4;
        list->data = ncc_realloc(list->data, new_cap * sizeof(uint64_t));
        list->cap = new_cap;
    }

    list->data[list->len++] = value;
}

static void
collect_nt_children(ncc_parse_tree_t *node, const char *name,
                    parse_tree_list_t *out)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        parse_tree_list_push(out, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_nt_children(ncc_tree_child(node, i), name, out);
    }
}

static void
collect_leaf_nodes(ncc_parse_tree_t *node, parse_tree_list_t *out)
{
    if (!node) {
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        parse_tree_list_push(out, node);
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_leaf_nodes(ncc_tree_child(node, i), out);
    }
}

static bool
child_index(ncc_parse_tree_t *parent, ncc_parse_tree_t *child,
            size_t *idx_out)
{
    if (!parent || !child || ncc_tree_is_leaf(parent)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(parent);
    for (size_t i = 0; i < nc; i++) {
        if (ncc_tree_child(parent, i) == child) {
            if (idx_out) {
                *idx_out = i;
            }
            return true;
        }
    }

    return false;
}

static ncc_token_info_t *
first_leaf_token(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_tree_leaf_value(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_token_info_t *tok = first_leaf_token(ncc_tree_child(node, i));
        if (tok) {
            return tok;
        }
    }

    return nullptr;
}

static const char *
token_file(ncc_token_info_t *tok)
{
    if (!tok || !ncc_option_is_set(tok->file)) {
        return nullptr;
    }

    ncc_string_t file = ncc_option_get(tok->file);
    return file.data;
}

static bool
token_file_looks_system_header(ncc_token_info_t *tok)
{
    const char *file = token_file(tok);
    if (!file) {
        return false;
    }

    return strstr(file, "/usr/include/") != nullptr
        || strstr(file, "/usr/lib/clang/") != nullptr
        || strstr(file, "/System/Library/Frameworks/") != nullptr
        || strstr(file, "/opt/homebrew/") != nullptr
        || strstr(file, "/usr/local/") != nullptr;
}

static bool
node_starts_in_system_header(ncc_parse_tree_t *node)
{
    ncc_token_info_t *tok = first_leaf_token(node);
    return tok && (tok->system_header || token_file_looks_system_header(tok));
}

static bool
node_direct_child_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (child && ncc_tree_is_leaf(child)
            && ncc_xform_leaf_text_eq(child, text)) {
            return true;
        }
    }

    return false;
}

static bool
node_first_child_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node || ncc_tree_is_leaf(node) || ncc_tree_num_children(node) == 0) {
        return false;
    }

    ncc_parse_tree_t *child = ncc_tree_child(node, 0);
    return child && ncc_tree_is_leaf(child)
        && ncc_xform_leaf_text_eq(child, text);
}

static bool
node_uses_computed_goto(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    if (ncc_xform_nt_name_is(node, "jump_statement")
        && node_first_child_leaf_text(node, "goto")
        && node_direct_child_leaf_text(node, "*")) {
        return true;
    }

    if (ncc_xform_nt_name_is(node, "unary_expression")
        && node_first_child_leaf_text(node, "&&")) {
        return true;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (node_uses_computed_goto(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

static bool
node_mentions_manual_gc_stack_api(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, "n00b_gc_stack_push")
            || ncc_xform_leaf_text_eq(node, "n00b_gc_stack_pop");
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (node_mentions_manual_gc_stack_api(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

static ncc_parse_tree_t *
first_leaf_node(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *leaf = first_leaf_node(ncc_tree_child(node, i));
        if (leaf) {
            return leaf;
        }
    }

    return nullptr;
}

static void
add_token_trivia(ncc_token_info_t *tok, const char *text, bool leading)
{
    if (!tok || !text) {
        return;
    }

    ncc_trivia_t *trivia = ncc_alloc(ncc_trivia_t);
    trivia->text = ncc_string_from_cstr(text);

    if (leading) {
        trivia->next = tok->leading_trivia;
        tok->leading_trivia = trivia;
    }
    else {
        trivia->next = tok->trailing_trivia;
        tok->trailing_trivia = trivia;
    }
}

static void
add_generated_spacing(ncc_parse_tree_t *node, bool leading)
{
    if (leading) {
        add_token_trivia(first_leaf_token(node), "\n", true);
    }

    add_token_trivia(ncc_xform_find_last_leaf_token(node), "\n", false);
}

static bool
is_group_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return pn.group_top || pn.group_item
        || (pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$');
}

static ncc_parse_tree_t *
node_parent(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return (ncc_parse_tree_t *)pn.parent;
}

static ncc_parse_tree_t *
declaration_block_item(ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *decl = ncc_xform_find_ancestor(node, "declaration");
    if (!decl) {
        return nullptr;
    }

    ncc_parse_tree_t *cur = decl;
    for (;;) {
        ncc_parse_tree_t *parent = node_parent(cur);
        if (!parent) {
            return nullptr;
        }

        if (ncc_xform_nt_name_is(parent, "block_item")) {
            return parent;
        }

        if (!is_group_node(parent)) {
            return nullptr;
        }

        cur = parent;
    }
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

static int32_t
first_positive_token_index(ncc_parse_tree_t *node)
{
    if (!node) {
        return 0;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        return tok && tok->index > 0 ? tok->index : 0;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        int32_t index = first_positive_token_index(ncc_tree_child(node, i));
        if (index > 0) {
            return index;
        }
    }

    return 0;
}

static int32_t
node_start_index(ncc_parse_tree_t *node)
{
    int32_t index = first_positive_token_index(node);
    if (index > 0) {
        return index;
    }

    ncc_token_info_t *tok = first_leaf_token(node);
    return tok ? tok->index : 0;
}

static char *
direct_goto_label_name(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "jump_statement")
        || !node_first_child_leaf_text(node, "goto")
        || node_direct_child_leaf_text(node, "*")) {
        return nullptr;
    }

    ncc_parse_tree_t *id = first_descendant_nt(node, "identifier");
    return id ? node_text(id) : nullptr;
}

static char *
statement_label_name(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "label")
        || contains_leaf_text(node, "case")
        || contains_leaf_text(node, "default")) {
        return nullptr;
    }

    ncc_parse_tree_t *id = first_descendant_nt(node, "identifier");
    return id ? node_text(id) : nullptr;
}

static bool
has_matching_label_after(ncc_parse_tree_t *node, const char *name,
                         int32_t anchor_start)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    char *label = statement_label_name(node);
    if (label) {
        bool match = node_start_index(node) > anchor_start
                  && strcmp(label, name) == 0;
        ncc_free(label);
        if (match) {
            return true;
        }
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (has_matching_label_after(ncc_tree_child(node, i), name,
                                     anchor_start)) {
            return true;
        }
    }

    return false;
}

static bool
has_forward_goto_bypassing_anchor_r(ncc_parse_tree_t *root,
                                    ncc_parse_tree_t *node,
                                    int32_t anchor_start)
{
    if (!root || !node || ncc_tree_is_leaf(node)) {
        return false;
    }

    char *name = direct_goto_label_name(node);
    if (name) {
        bool bypass = node_start_index(node) < anchor_start
                   && has_matching_label_after(root, name, anchor_start);
        ncc_free(name);
        if (bypass) {
            return true;
        }
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (has_forward_goto_bypassing_anchor_r(root, ncc_tree_child(node, i),
                                                anchor_start)) {
            return true;
        }
    }

    return false;
}

static bool
has_forward_goto_bypassing_anchor(ncc_parse_tree_t *fn,
                                  ncc_parse_tree_t *anchor)
{
    return fn && anchor
        && has_forward_goto_bypassing_anchor_r(fn, fn,
                                               node_start_index(anchor));
}

static bool
node_first_leaf_text_eq(ncc_parse_tree_t *node, const char *text)
{
    ncc_token_info_t *tok = first_leaf_token(node);
    if (!tok || !ncc_option_is_set(tok->value)) {
        return false;
    }

    ncc_string_t value = ncc_option_get(tok->value);
    return value.data && strcmp(value.data, text) == 0;
}

static bool
node_starts_with_switch_case_label(ncc_parse_tree_t *node)
{
    return node_first_leaf_text_eq(node, "case")
        || node_first_leaf_text_eq(node, "default");
}

static bool
block_has_case_label_item(ncc_parse_tree_t *anchor)
{
    ncc_parse_tree_t *container = node_parent(anchor);

    if (!container) {
        return false;
    }

    size_t nc = ncc_tree_num_children(container);
    for (size_t i = 0; i < nc; i++) {
        if (node_starts_with_switch_case_label(ncc_tree_child(container, i))) {
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

static int
pointer_depth_in_declarator(ncc_parse_tree_t *node)
{
    return pointer_depth_in_type_node(node);
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

static char *
declarator_name(ncc_parse_tree_t *node)
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
            return copy_cstr(text);
        }
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *candidate = declarator_name(ncc_tree_child(node, i));
        if (candidate) {
            return candidate;
        }
    }
    return nullptr;
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

[[noreturn]] static void
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
path_contains_portable(const char *path, const char *needle)
{
    if (!path || !needle) {
        return false;
    }

    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return false;
    }

    for (const char *p = path; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            char pc = p[i] == '\\' ? '/' : p[i];
            char nc = needle[i] == '\\' ? '/' : needle[i];
            if (pc != nc) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

static bool
token_source_is_sanctioned_stw_header(ncc_parse_tree_t *leaf)
{
    if (!leaf || !ncc_tree_is_leaf(leaf)) {
        return false;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(leaf);
    const char       *file = token_file(tok);
    return path_contains_portable(file, "include/core/stw.h");
}

static bool
leaf_text_is_nonlocal_exit_call(const char *text)
{
    return text
        && (strcmp(text, "setjmp") == 0
            || strcmp(text, "_setjmp") == 0
            || strcmp(text, "__sigsetjmp") == 0
            || strcmp(text, "sigsetjmp") == 0
            || strcmp(text, "__builtin_setjmp") == 0
            || strcmp(text, "longjmp") == 0
            || strcmp(text, "_longjmp") == 0
            || strcmp(text, "siglongjmp") == 0
            || strcmp(text, "__builtin_longjmp") == 0);
}

static bool
leaf_text_is_setjmp_call(const char *text)
{
    return text
        && (strcmp(text, "setjmp") == 0
            || strcmp(text, "_setjmp") == 0
            || strcmp(text, "__sigsetjmp") == 0
            || strcmp(text, "sigsetjmp") == 0
            || strcmp(text, "__builtin_setjmp") == 0);
}

static ncc_parse_tree_t *
nonlocal_exit_call_leaf(ncc_parse_tree_t *node, ncc_parse_tree_t **call_out)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "postfix_expression")
        || ncc_tree_num_children(node) < 3
        || !ncc_xform_leaf_text_eq(ncc_tree_child(node, 1), "(")) {
        return nullptr;
    }

    ncc_parse_tree_t *callee = first_leaf_node(ncc_tree_child(node, 0));
    if (!callee || !ncc_tree_is_leaf(callee)) {
        return nullptr;
    }

    const char *name = ncc_xform_leaf_text(callee);
    if (!leaf_text_is_nonlocal_exit_call(name)) {
        return nullptr;
    }

    if (call_out) {
        *call_out = node;
    }
    return callee;
}

static bool
compound_looks_like_stw_run_blocking_macro(ncc_parse_tree_t *compound)
{
    if (!compound || ncc_tree_is_leaf(compound)
        || !ncc_xform_nt_name_is(compound, "compound_statement")) {
        return false;
    }

    char *text = node_text(compound);
    bool  result =
        strncmp(text, "{ jmp_buf save_state", strlen("{ jmp_buf save_state"))
            == 0
        && contains_leaf_text(compound, "setjmp")
        && contains_leaf_text(compound, "n00b_capture_stack_top")
        && contains_leaf_text(compound, "_n00b_thread_suspend")
        && contains_leaf_text(compound, "_n00b_thread_resume")
        && contains_leaf_text(compound, "longjmp")
        && contains_leaf_text(compound, "n00b_thread_checkin");
    ncc_free(text);
    return result;
}

static bool
call_first_argument_is(ncc_parse_tree_t *call, const char *name)
{
    if (!call || !name || ncc_tree_is_leaf(call)
        || ncc_tree_num_children(call) < 3) {
        return false;
    }

    ncc_parse_tree_t *args = ncc_tree_child(call, 2);
    ncc_parse_tree_t *leaf = first_leaf_node(args);
    return leaf && ncc_tree_is_leaf(leaf)
        && ncc_xform_leaf_text_eq(leaf, name);
}

static bool
setjmp_uses_n00b_nonlocal_api(ncc_parse_tree_t *call)
{
    if (!call || ncc_tree_is_leaf(call) || ncc_tree_num_children(call) < 3) {
        return false;
    }

    ncc_parse_tree_t *args = ncc_tree_child(call, 2);
    parse_tree_list_t leaves = {0};
    collect_leaf_nodes(args, &leaves);

    size_t start = 0;
    size_t end   = leaves.len;
    while (start < end && ncc_xform_leaf_text_eq(leaves.data[start], "(")) {
        start++;
    }
    while (end > start && ncc_xform_leaf_text_eq(leaves.data[end - 1], ")")) {
        end--;
    }

    bool result = false;
    if (end > start + 4
        && ncc_xform_leaf_text_eq(leaves.data[start],
                                  "n00b_gc_stack_prepare_jmp")
        && ncc_xform_leaf_text_eq(leaves.data[start + 1], "(")) {
        int    depth = 0;
        size_t close = (size_t)-1;
        for (size_t i = start + 1; i < end; i++) {
            if (ncc_xform_leaf_text_eq(leaves.data[i], "(")) {
                depth++;
            }
            else if (ncc_xform_leaf_text_eq(leaves.data[i], ")")) {
                depth--;
                if (depth == 0) {
                    close = i;
                    break;
                }
            }
        }

        result = close != (size_t)-1
              && close + 3 == end
              && ncc_xform_leaf_text_eq(leaves.data[close + 1], "->")
              && ncc_xform_leaf_text_eq(leaves.data[close + 2],
                                        "n00b_jmp_env");
    }

    ncc_free(leaves.data);
    return result;
}

static bool
nonlocal_exit_call_is_sanctioned(ncc_parse_tree_t *call,
                                 ncc_parse_tree_t *leaf,
                                 bool in_stw_run_blocking_macro)
{
    if (token_source_is_sanctioned_stw_header(leaf)) {
        return true;
    }

    const char *name = ncc_xform_leaf_text(leaf);
    if (leaf_text_is_setjmp_call(name) && setjmp_uses_n00b_nonlocal_api(call)) {
        return true;
    }

    return in_stw_run_blocking_macro
        && call_first_argument_is(call, "save_state");
}

static ncc_parse_tree_t *
find_unsafe_nonlocal_exit_call_r(ncc_parse_tree_t *node, const char **name_out,
                                 bool in_stw_run_blocking_macro)
{
    if (!node) {
        return nullptr;
    }

    if (compound_looks_like_stw_run_blocking_macro(node)) {
        in_stw_run_blocking_macro = true;
    }

    ncc_parse_tree_t *call = nullptr;
    ncc_parse_tree_t *leaf = nonlocal_exit_call_leaf(node, &call);
    if (leaf) {
        if (!nonlocal_exit_call_is_sanctioned(call, leaf,
                                              in_stw_run_blocking_macro)) {
            if (name_out) {
                *name_out = ncc_xform_leaf_text(leaf);
            }
            return leaf;
        }
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found =
            find_unsafe_nonlocal_exit_call_r(ncc_tree_child(node, i),
                                             name_out,
                                             in_stw_run_blocking_macro);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static ncc_parse_tree_t *
find_unsafe_nonlocal_exit_call(ncc_parse_tree_t *node, const char **name_out)
{
    return find_unsafe_nonlocal_exit_call_r(node, name_out, false);
}

static bool
decl_is_stack_storage(ncc_parse_tree_t *decl_specs)
{
    return !contains_leaf_text(decl_specs, "static")
        && !contains_leaf_text(decl_specs, "extern")
        && !contains_leaf_text(decl_specs, "typedef");
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
array_bound_allows_static_sizeof(const char *text)
{
    char *trimmed = trim_copy(text);
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
            char *text = node_text(bound);
            bool  ok   = parse_positive_integer_literal(text, &count);
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
has_parenthesized_pointer_array(ncc_parse_tree_t *declarator)
{
    char *text = node_text(declarator);
    bool  result = strstr(text, "(*") != nullptr || strstr(text, "( *")
                                                     != nullptr;
    ncc_free(text);
    return result;
}

static ncc_dict_t *
gc_aggregate_types(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_aggregate_types : nullptr;
}

static ncc_dict_t *
gc_pointer_typedefs(ncc_xform_ctx_t *ctx)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    return data ? &data->gc_pointer_typedefs : nullptr;
}

static char *
struct_or_union_kind_text(ncc_parse_tree_t *su)
{
    ncc_parse_tree_t *kind = first_descendant_nt(su,
                                                 "_kw_kw_struct_or_union");
    return kind ? node_text(kind) : nullptr;
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

static aggregate_type_info_t *
lookup_aggregate_type(ncc_xform_ctx_t *ctx, const char *key)
{
    ncc_dict_t *types = gc_aggregate_types(ctx);
    if (!types || !key) {
        return nullptr;
    }

    bool found = false;
    aggregate_type_info_t *info = ncc_dict_get(types, (void *)key, &found);
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
    aggregate_type_info_t *info = lookup_aggregate_type(ctx, key);
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

static bool typedef_name_is_pointer(ncc_xform_ctx_t *ctx, const char *name);

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

static ncc_parse_tree_t *
aggregate_spec_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    if (su) {
        return resolve_aggregate_specifier(ctx, su);
    }

    char *td_name = first_typedef_name_text(specs);
    aggregate_type_info_t *info = lookup_aggregate_type(ctx, td_name);
    if (!info && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, false, true);
        info    = lookup_aggregate_type(ctx, td_name);
    }
    ncc_free(td_name);
    return info ? info->specifier : nullptr;
}

static aggregate_type_info_t *
aggregate_info_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    if (su) {
        char *key = aggregate_key_from_specifier(su);
        aggregate_type_info_t *info = lookup_aggregate_type(ctx, key);
        ncc_free(key);
        if (info) {
            return info;
        }
    }

    char *td_name = first_typedef_name_text(specs);
    aggregate_type_info_t *info = lookup_aggregate_type(ctx, td_name);
    if (!info && !td_name) {
        td_name = first_known_type_alias_text(ctx, specs, false, true);
        info    = lookup_aggregate_type(ctx, td_name);
    }
    ncc_free(td_name);
    return info;
}

static bool
specs_have_atomic_type_wrapper(ncc_parse_tree_t *node)
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
        if (specs_have_atomic_type_wrapper(ncc_tree_child(node, i))) {
            return true;
        }
    }

    return false;
}

static char *
aggregate_offset_type_from_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs)
{
    ncc_parse_tree_t *su = first_descendant_nt(specs,
                                               "struct_or_union_specifier");
    if (su) {
        char *key = aggregate_key_from_specifier(su);
        if (key) {
            return key;
        }
    }

    char *td_name = first_typedef_name_text(specs);
    if (td_name) {
        return td_name;
    }

    return first_known_type_alias_text(ctx, specs, false, true);
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

static bool
typedef_name_is_pointer(ncc_xform_ctx_t *ctx, const char *name)
{
    ncc_dict_t *ptrs = gc_pointer_typedefs(ctx);
    if (!ptrs || !name) {
        return false;
    }

    bool found = false;
    (void)ncc_dict_get(ptrs, (void *)name, &found);
    return found;
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

static bool
array_literal_dimensions(ncc_parse_tree_t *declarator,
                         ncc_parse_tree_t *decl_node,
                         uint64_list_t *dims)
{
    char *text = node_text(declarator);
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
        char *trimmed = trim_copy(bound);
        if (trimmed[0] == '\0' && dims->len == 0) {
            ok = infer_array_bound_from_initializer(decl_node, &count);
        }
        else {
            ok = parse_positive_integer_literal(trimmed, &count);
        }
        ncc_free(trimmed);
        ncc_free(bound);
        if (!ok) {
            ncc_free(text);
            return false;
        }

        uint64_list_push(dims, count);
        p = close + 1;
    }

    ncc_free(text);
    return dims->len > 0;
}

static char *
num_words_expr_for_pointer_array(ncc_parse_tree_t *declarator,
                                 const char *expr,
                                 ncc_parse_tree_t **bad_array)
{
    uint64_t product    = 1;
    bool     use_sizeof = false;
    if (!array_bound_product(declarator, &product, &use_sizeof, bad_array)) {
        return nullptr;
    }

    if (use_sizeof) {
        return format_cstr("(sizeof(%s)/sizeof(void *))", expr);
    }

    return format_cstr("%llu", (unsigned long long)product);
}

static void
collect_gc_type_info(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu)
{
    ncc_layout_collect_type_info(ctx, tu);
}

static ncc_gc_stack_root_t *
record_root(ncc_xform_ctx_t *ctx, const char *function, const char *name,
            ncc_parse_tree_t *type_node, ncc_gc_stack_root_kind_t kind,
            ncc_gc_stack_root_shape_t shape, uint64_t num_words,
            const char *address_expr, const char *num_words_expr,
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
        .address_expr  = copy_cstr(address_expr ? address_expr : name),
        .num_words_expr = num_words_expr ? copy_cstr(num_words_expr)
                                         : nullptr,
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

static ncc_gc_stack_root_t *
record_root_expr(ncc_xform_ctx_t *ctx, const char *function,
                 const char *name, ncc_parse_tree_t *type_node,
                 ncc_gc_stack_root_kind_t kind,
                 ncc_gc_stack_root_shape_t shape,
                 const char *address_expr, const char *num_words_expr,
                 ncc_parse_tree_t *scope, ncc_parse_tree_t *decl_node)
{
    uint64_t num_words = 0;

    if (!num_words_expr || parse_positive_integer_literal(num_words_expr,
                                                          &num_words)) {
        if (!num_words_expr) {
            num_words = 1;
        }
    }

    return record_root(ctx, function, name, type_node, kind, shape,
                       num_words, address_expr, num_words_expr, scope,
                       decl_node);
}

static char *
append_offset_field(const char *path, const char *field)
{
    if (path && *path) {
        return format_cstr("%s.%s", path, field);
    }

    return copy_cstr(field);
}

static char *
field_address_expr(const char *field_expr, const char *atomic_base_expr,
                   const char *atomic_type, const char *atomic_field_path)
{
    if (atomic_base_expr && atomic_type && atomic_field_path
        && *atomic_field_path) {
        return format_cstr("((char *)&%s + __builtin_offsetof(%s, %s))",
                           atomic_base_expr, atomic_type, atomic_field_path);
    }

    return format_cstr("&%s", field_expr);
}

static char *
atomic_offset_type_for_specs(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs,
                             aggregate_type_info_t *info)
{
    if (info && info->is_atomic) {
        return copy_cstr(info->offset_type);
    }

    if (specs_have_atomic_type_wrapper(specs)) {
        return aggregate_offset_type_from_specs(ctx, specs);
    }

    return nullptr;
}

typedef struct {
    const char *base_expr;
    const char *type;
    const char *path;
    char       *owned_type;
} atomic_child_context_t;

static atomic_child_context_t
atomic_child_context_for_field(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *specs,
                               aggregate_type_info_t *info,
                               const char *parent_base_expr,
                               const char *parent_type,
                               const char *parent_path,
                               const char *field_expr,
                               const char *field_path)
{
    atomic_child_context_t result = {
        .base_expr  = parent_base_expr,
        .type       = parent_type,
        .path       = parent_path,
        .owned_type = atomic_offset_type_for_specs(ctx, specs, info),
    };

    if (result.owned_type) {
        result.base_expr = field_expr;
        result.type      = result.owned_type;
        result.path      = "";
    }
    else if (parent_base_expr) {
        result.path = field_path;
    }

    return result;
}

static size_t expand_aggregate_fields(ncc_xform_ctx_t *ctx,
                                      const char *function,
                                      const char *root_name,
                                      const char *base_expr,
                                      ncc_parse_tree_t *aggregate_spec,
                                      ncc_gc_stack_root_kind_t kind,
                                      ncc_parse_tree_t *scope,
                                      ncc_parse_tree_t *decl_node,
                                      int depth,
                                      const char *atomic_base_expr,
                                      const char *atomic_type,
                                      const char *atomic_path);

static size_t
expand_aggregate_array_elements(ncc_xform_ctx_t *ctx, const char *function,
                                const char *root_name, const char *array_expr,
                                ncc_parse_tree_t *aggregate_spec,
                                uint64_list_t *dims, size_t dim_index,
                                ncc_gc_stack_root_kind_t kind,
                                ncc_parse_tree_t *scope,
                                ncc_parse_tree_t *decl_node,
                                int depth,
                                const char *atomic_base_expr,
                                const char *atomic_type,
                                const char *atomic_path)
{
    if (dim_index == dims->len) {
        const char *field_atomic_base = atomic_base_expr;
        const char *field_atomic_path = atomic_path;
        if (!field_atomic_base && atomic_type) {
            field_atomic_base = array_expr;
            field_atomic_path = "";
        }

        return expand_aggregate_fields(ctx, function, root_name, array_expr,
                                       aggregate_spec, kind, scope, decl_node,
                                       depth + 1, field_atomic_base,
                                       atomic_type, field_atomic_path);
    }

    size_t total = 0;
    for (uint64_t i = 0; i < dims->data[dim_index]; i++) {
        char *element = format_cstr("%s[%llu]", array_expr,
                                    (unsigned long long)i);
        total += expand_aggregate_array_elements(ctx, function, root_name,
                                                 element, aggregate_spec,
                                                 dims, dim_index + 1, kind,
                                                 scope, decl_node, depth,
                                                 atomic_base_expr, atomic_type,
                                                 atomic_path);
        ncc_free(element);
    }

    return total;
}

// Returns true if `aggregate_spec` (a struct/union specifier) contains any
// pointer-bearing field, recursively walking nested aggregate members.
//
// Used by `expand_aggregate_array` to decide what to do when an aggregate
// array's bounds are not a compile-time integer literal.  If the element
// type has no pointer fields, the array contributes no GC roots regardless
// of its length and we silently skip it.  Otherwise we fall back to a
// conservative whole-buffer scan whose size is computed at runtime from
// the VLA's sizeof.
//
// The walk mirrors `expand_member_declaration` / `expand_member_declarator`
// at a structural level, but only checks for pointer presence; it never
// records roots.  Bounded recursion depth matches `expand_aggregate_fields`.
static bool aggregate_spec_has_pointer_fields(ncc_xform_ctx_t *ctx,
                                              ncc_parse_tree_t *aggregate_spec,
                                              int depth);

static bool
member_declaration_has_pointer_field(ncc_xform_ctx_t *ctx,
                                     ncc_parse_tree_t *member,
                                     int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return false;
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");
    if (!member_list) {
        // Anonymous member: either a bare pointer (rare) or an inline
        // anonymous aggregate; recurse into the latter.
        int ptr_depth = pointer_depth_for_specs(ctx, member_specs);
        if (ptr_depth > 0) {
            return true;
        }
        ncc_parse_tree_t *nested = aggregate_spec_from_specs(ctx, member_specs);
        if (nested) {
            return aggregate_spec_has_pointer_fields(ctx, nested, depth + 1);
        }
        return false;
    }

    parse_tree_list_t declarators = {0};
    collect_nt_children(member_list, "member_declarator", &declarators);

    bool found = false;
    for (size_t i = 0; i < declarators.len && !found; i++) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
            declarators.data[i], "declarator");
        if (!declarator) {
            continue;
        }

        int ptr_depth = pointer_depth_for_declarator(ctx, member_specs,
                                                     declarator);
        if (ptr_depth > 0) {
            found = true;
            break;
        }

        // No pointer on this declarator; if the field's type is itself an
        // aggregate, recurse.
        ncc_parse_tree_t *nested = aggregate_spec_from_specs(ctx, member_specs);
        if (nested && aggregate_spec_has_pointer_fields(ctx, nested,
                                                        depth + 1)) {
            found = true;
        }
    }

    ncc_free(declarators.data);
    return found;
}

static bool
aggregate_spec_has_pointer_fields(ncc_xform_ctx_t *ctx,
                                  ncc_parse_tree_t *aggregate_spec,
                                  int depth)
{
    if (!aggregate_spec || depth > 64) {
        return false;
    }

    aggregate_spec = resolve_aggregate_specifier(ctx, aggregate_spec);
    if (!aggregate_spec) {
        // Opaque / unresolved: treat conservatively as if it could carry
        // pointers, matching the existing warn-and-skip stance for
        // incomplete types in `expand_aggregate_fields`.
        return true;
    }

    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        aggregate_spec, "member_declaration_list");
    if (!members) {
        return true;
    }

    parse_tree_list_t member_decls = {0};
    collect_nt_children(members, "member_declaration", &member_decls);

    bool found = false;
    for (size_t i = 0; i < member_decls.len && !found; i++) {
        if (member_declaration_has_pointer_field(ctx, member_decls.data[i],
                                                 depth)) {
            found = true;
        }
    }

    ncc_free(member_decls.data);
    return found;
}

static size_t
expand_aggregate_array(ncc_xform_ctx_t *ctx, const char *function,
                       const char *root_name, const char *array_expr,
                       ncc_parse_tree_t *aggregate_spec,
                       ncc_parse_tree_t *declarator,
                       ncc_gc_stack_root_kind_t kind,
                       ncc_parse_tree_t *scope,
                       ncc_parse_tree_t *decl_node,
                       int depth,
                       const char *atomic_base_expr,
                       const char *atomic_type,
                       const char *atomic_path)
{
    uint64_list_t dims = {0};
    if (!array_literal_dimensions(declarator, decl_node, &dims)) {
        ncc_xform_data_t *data = ncc_xform_get_data(ctx);
        ncc_free(dims.data);
        if (data && data->gc_stack_maps_relaxed) {
            return 0;
        }

        // Non-literal bound: either a true VLA (e.g. `T arr[n]`) or a
        // bound expressed as a non-trivial compile-time constant
        // expression that the transform doesn't constant-fold (e.g.
        // `T arr[((size_t)1024u)]` after macro expansion).  Either way
        // we can't statically enumerate per-element field roots.
        //
        // Two subcases (D-033):
        //
        //   1. The element type has no pointer fields anywhere in its
        //      transitive layout.  The array contributes no GC roots
        //      regardless of length; silently skip it.
        //
        //   2. The element type IS pointer-bearing.  We emit ONE
        //      conservative root covering the entire array storage:
        //      address = &arr[0], num_words = sizeof(arr)/sizeof(void*).
        //      Every word in the array is scanned as a potential
        //      pointer; non-pointer interleaved bytes are harmless
        //      (the collector ignores misaligned / non-arena bits).
        //
        //      For a true VLA, sizeof(arr) is a runtime expression and
        //      cannot appear in a `static const` slot-table initializer.
        //      The root is flagged `num_words_runtime`, and the emitter
        //      (build_frame_source) generates a non-static slot table /
        //      map for any group that contains such a root.
        if (!aggregate_spec_has_pointer_fields(ctx, aggregate_spec, 0)) {
            return 0;
        }

        // array_expr at this entry-point is the locally-declared array's
        // identifier (set by classify_declarator / expand_aggregate_fields
        // at depth=0).  Use it directly for sizeof / address.
        //
        // The divisor's `sizeof(void *)` is wrapped in extra parentheses to
        // silence clang's `-Wsizeof-array-div` warning, which fires when the
        // array's element type is not `void *` (here it is an aggregate).
        // Clang's diagnostic note explicitly recommends this form.
        char *addr  = format_cstr("&%s[0]", array_expr);
        char *words = format_cstr("(sizeof(%s)/(sizeof(void *)))", array_expr);

        ncc_gc_stack_root_t *root = record_root_expr(ctx, function, root_name,
                                                     aggregate_spec, kind,
                                                     NCC_GC_STACK_ROOT_POINTER_ARRAY,
                                                     addr, words, scope,
                                                     decl_node);
        if (root) {
            root->num_words_runtime = true;
        }

        ncc_free(words);
        ncc_free(addr);
        return 1;
    }

    size_t total = expand_aggregate_array_elements(ctx, function, root_name,
                                                   array_expr, aggregate_spec,
                                                   &dims, 0, kind, scope,
                                                   decl_node, depth,
                                                   atomic_base_expr,
                                                   atomic_type, atomic_path);
    ncc_free(dims.data);
    return total;
}

static size_t
expand_member_declarator(ncc_xform_ctx_t *ctx, const char *function,
                         const char *root_name, const char *base_expr,
                         ncc_parse_tree_t *member_specs,
                         ncc_parse_tree_t *member_declarator,
                         ncc_gc_stack_root_kind_t kind,
                         ncc_parse_tree_t *scope,
                         ncc_parse_tree_t *decl_node,
                         int depth,
                         const char *atomic_base_expr,
                         const char *atomic_type,
                         const char *atomic_path)
{
    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(member_declarator,
                                                           "declarator");
    if (!declarator) {
        return 0;
    }

    char *field_name = declarator_name(declarator);
    if (!field_name) {
        return 0;
    }

    char *field_expr = format_cstr("%s.%s", base_expr, field_name);
    char *field_path = append_offset_field(atomic_path, field_name);
    int   ptr_depth  = pointer_depth_for_declarator(ctx, member_specs,
                                                    declarator);

    bool is_array = first_descendant_nt(declarator, "array_declarator")
                 != nullptr;
    size_t roots = 0;

    if (ptr_depth > 0) {
        char *addr = field_address_expr(field_expr, atomic_base_expr,
                                        atomic_type, field_path);
        char *words = nullptr;
        ncc_gc_stack_root_shape_t shape = NCC_GC_STACK_ROOT_POINTER;

        if (is_array) {
            // Flexible array members have no by-value stack storage to scan.
            if (has_unbounded_array_declarator(declarator)) {
                ncc_free(addr);
                ncc_free(field_path);
                ncc_free(field_expr);
                ncc_free(field_name);
                return 0;
            }

            ncc_parse_tree_t *bad_array = nullptr;
            words = num_words_expr_for_pointer_array(declarator, field_expr,
                                                     &bad_array);
            if (!words) {
                ncc_xform_data_t *data = ncc_xform_get_data(ctx);
                if (data && data->gc_stack_maps_relaxed) {
                    ncc_free(addr);
                    ncc_free(field_path);
                    ncc_free(field_expr);
                    ncc_free(field_name);
                    return 0;
                }
                gc_stack_errorf(bad_array ? bad_array : declarator,
                                "GC stack-map root '%s' in function '%s' uses "
                                "a variable-length or incomplete pointer array "
                                "field; only fixed-size stack roots are "
                                "supported",
                                field_expr, function);
            }
            shape = NCC_GC_STACK_ROOT_POINTER_ARRAY;
        }
        else {
            words = copy_cstr("1");
        }

        record_root_expr(ctx, function, field_expr, member_specs, kind, shape,
                         addr, words, scope, decl_node);
        roots++;
        ncc_free(addr);
        ncc_free(words);
    }
    else {
        aggregate_type_info_t *nested_info = aggregate_info_from_specs(
            ctx, member_specs);
        ncc_parse_tree_t *nested = nested_info
                                     ? nested_info->specifier
                                     : aggregate_spec_from_specs(ctx,
                                                                member_specs);
        if (nested) {
            atomic_child_context_t child = atomic_child_context_for_field(
                ctx, member_specs, nested_info, atomic_base_expr, atomic_type,
                atomic_path, field_expr, field_path);

            if (is_array) {
                roots += expand_aggregate_array(ctx, function, root_name,
                                                field_expr, nested,
                                                declarator, kind, scope,
                                                decl_node, depth + 1,
                                                child.base_expr,
                                                child.type,
                                                child.path);
            }
            else {
                roots += expand_aggregate_fields(ctx, function, root_name,
                                                 field_expr, nested, kind,
                                                 scope, decl_node, depth + 1,
                                                 child.base_expr,
                                                 child.type,
                                                 child.path);
            }
            ncc_free(child.owned_type);
        }
    }

    ncc_free(field_expr);
    ncc_free(field_path);
    ncc_free(field_name);
    return roots;
}

static size_t
expand_member_declaration(ncc_xform_ctx_t *ctx, const char *function,
                          const char *root_name, const char *base_expr,
                          ncc_parse_tree_t *member,
                          ncc_gc_stack_root_kind_t kind,
                          ncc_parse_tree_t *scope,
                          ncc_parse_tree_t *decl_node,
                          int depth,
                          const char *atomic_base_expr,
                          const char *atomic_type,
                          const char *atomic_path)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return 0;
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");
    if (!member_list) {
        int ptr_depth = pointer_depth_for_specs(ctx, member_specs);

        if (ptr_depth > 0) {
            char *field_name = implicit_member_field_name(member, member_specs);
            if (!field_name) {
                return 0;
            }

            char *field_expr = format_cstr("%s.%s", base_expr, field_name);
            char *field_path = append_offset_field(atomic_path, field_name);
            char *addr       = field_address_expr(field_expr, atomic_base_expr,
                                                  atomic_type, field_path);
            char *words      = copy_cstr("1");

            record_root_expr(ctx, function, field_expr, member_specs, kind,
                             NCC_GC_STACK_ROOT_POINTER, addr, words, scope,
                             decl_node);

            ncc_free(words);
            ncc_free(addr);
            ncc_free(field_path);
            ncc_free(field_expr);
            ncc_free(field_name);
            return 1;
        }

        aggregate_type_info_t *anonymous_info = aggregate_info_from_specs(
            ctx, member_specs);
        ncc_parse_tree_t *anonymous = anonymous_info
                                        ? anonymous_info->specifier
                                        : aggregate_spec_from_specs(ctx,
                                                                   member_specs);
        if (!anonymous) {
            return 0;
        }

        char *field_name = implicit_member_field_name(member, member_specs);
        if (!field_name) {
            return expand_aggregate_fields(ctx, function, root_name,
                                           base_expr, anonymous, kind, scope,
                                           decl_node, depth + 1,
                                           atomic_base_expr, atomic_type,
                                           atomic_path);
        }

        char *field_expr = format_cstr("%s.%s", base_expr, field_name);
        char *field_path = append_offset_field(atomic_path, field_name);
        atomic_child_context_t child = atomic_child_context_for_field(
            ctx, member_specs, anonymous_info, atomic_base_expr, atomic_type,
            atomic_path, field_expr, field_path);

        size_t roots = expand_aggregate_fields(ctx, function, root_name,
                                               field_expr, anonymous, kind,
                                               scope, decl_node, depth + 1,
                                               child.base_expr,
                                               child.type,
                                               child.path);
        ncc_free(child.owned_type);
        ncc_free(field_path);
        ncc_free(field_expr);
        ncc_free(field_name);
        return roots;
    }

    parse_tree_list_t declarators = {0};
    collect_nt_children(member_list, "member_declarator", &declarators);

    size_t roots = 0;
    for (size_t i = 0; i < declarators.len; i++) {
        roots += expand_member_declarator(ctx, function, root_name, base_expr,
                                          member_specs, declarators.data[i],
                                          kind, scope, decl_node, depth,
                                          atomic_base_expr, atomic_type,
                                          atomic_path);
    }

    ncc_free(declarators.data);
    return roots;
}

static size_t
expand_aggregate_fields(ncc_xform_ctx_t *ctx, const char *function,
                        const char *root_name, const char *base_expr,
                        ncc_parse_tree_t *aggregate_spec,
                        ncc_gc_stack_root_kind_t kind,
                        ncc_parse_tree_t *scope,
                        ncc_parse_tree_t *decl_node,
                        int depth,
                        const char *atomic_base_expr,
                        const char *atomic_type,
                        const char *atomic_path)
{
    if (depth > 64) {
        ncc_xform_data_t *data = ncc_xform_get_data(ctx);
        if (data && data->gc_stack_maps_relaxed) {
            return 0;
        }
        gc_stack_errorf(decl_node,
                        "GC stack-map aggregate root '%s' in function '%s' "
                        "has recursively nested aggregate fields",
                        root_name, function);
    }

    aggregate_spec = resolve_aggregate_specifier(ctx, aggregate_spec);
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        aggregate_spec, "member_declaration_list");
    if (!members) {
        ncc_xform_data_t *data = ncc_xform_get_data(ctx);
        if (data && data->gc_stack_maps_relaxed) {
            return 0;
        }
        gc_stack_errorf(decl_node,
                        "GC stack-map aggregate root '%s' in function '%s' "
                        "uses an incomplete or unresolved aggregate type",
                        root_name, function);
    }

    parse_tree_list_t member_decls = {0};
    collect_nt_children(members, "member_declaration", &member_decls);

    size_t roots = 0;
    for (size_t i = 0; i < member_decls.len; i++) {
        roots += expand_member_declaration(ctx, function, root_name, base_expr,
                                           member_decls.data[i], kind, scope,
                                           decl_node, depth,
                                           atomic_base_expr, atomic_type,
                                           atomic_path);
    }

    ncc_free(member_decls.data);
    return roots;
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

    ncc_xform_data_t *data = ncc_xform_get_data(ctx);

    if (cstr_has_prefix(function, "ncplane_")) {
        return;
    }

    int  ptr_depth = pointer_depth_for_declarator(ctx, decl_specs,
                                                  declarator);

    bool is_array  = first_descendant_nt(declarator, "array_declarator")
                  != nullptr;
    aggregate_type_info_t *aggregate_info = aggregate_info_from_specs(ctx,
                                                                      decl_specs);
    ncc_parse_tree_t *aggregate = aggregate_info
                                    ? aggregate_info->specifier
                                    : aggregate_spec_from_specs(ctx,
                                                               decl_specs);

    if (kind == NCC_GC_STACK_ROOT_PARAM && is_array) {
        ptr_depth = ptr_depth > 0 ? ptr_depth : 1;
        is_array  = false;
        aggregate = nullptr;
    }

    if (ptr_depth == 0 && !is_array && !aggregate) {
        return;
    }

    if (is_array && ptr_depth == 0 && !aggregate) {
        return;
    }

    char *name = declarator_name(declarator);
    if (!name) {
        gc_stack_errorf(declarator,
                        "GC stack-map root in function '%s' is unsupported: "
                        "pointer or aggregate parameter has no name",
                        function);
    }

    if (kind == NCC_GC_STACK_ROOT_LOCAL) {
        ncc_parse_tree_t *anchor = declaration_block_item(declarator);
        if (!anchor) {
            // If this is a pure-scalar aggregate (no pointer fields anywhere
            // in its transitive layout), the transform would record zero
            // roots even with a valid insertion point.  Skip silently rather
            // than forcing a rewrite of harmless for-init scaffolding (e.g.,
            // for-iteration "once" sentinels via inline anonymous structs).
            // Pointer-bearing aggregates still error: those need an anchor
            // so their fields can be scanned.
            if (ptr_depth == 0 && aggregate && !is_array
                && !aggregate_spec_has_pointer_fields(ctx, aggregate, 0)) {
                ncc_free(name);
                return;
            }
            if (data && data->gc_stack_maps_relaxed) {
                ncc_free(name);
                return;
            }
            gc_stack_errorf(declarator,
                            "GC stack-map root '%s' in function '%s' is declared "
                            "in an unsupported statement context; declare the root "
                            "as a block item before the statement",
                            name, function);
        }

        ncc_parse_tree_t *fn = ncc_xform_find_ancestor(decl_node,
                                                       "function_definition");
        if (block_has_case_label_item(anchor)
            || has_forward_goto_bypassing_anchor(fn, declarator)) {
            ncc_free(name);
            return;
        }
    }

    if (ptr_depth > 0) {
        ncc_gc_stack_root_shape_t shape = NCC_GC_STACK_ROOT_POINTER;
        char *num_words_expr = copy_cstr("1");

        if (is_array && ptr_depth > 0
            && has_parenthesized_pointer_array(declarator)) {
            if (data && data->gc_stack_maps_relaxed) {
                ncc_free(name);
                ncc_free(num_words_expr);
                return;
            }
            gc_stack_errorf(declarator,
                            "GC stack-map root '%s' in function '%s' uses a "
                            "parenthesized pointer/array declarator; spell the "
                            "root as a direct fixed-size pointer array or a "
                            "single pointer",
                            name, function);
        }

        if (is_array) {
            ncc_parse_tree_t *bad_array = nullptr;
            ncc_free(num_words_expr);
            num_words_expr = num_words_expr_for_pointer_array(declarator,
                                                              name,
                                                              &bad_array);
            if (!num_words_expr) {
                if (data && data->gc_stack_maps_relaxed) {
                    ncc_free(name);
                    return;
                }
                gc_stack_errorf(bad_array ? bad_array : declarator,
                                "GC stack-map root '%s' in function '%s' uses a "
                                "variable-length or incomplete array; only "
                                "fixed-size stack roots are supported",
                                name, function);
            }
            shape = NCC_GC_STACK_ROOT_POINTER_ARRAY;
        }

        char *address_expr = format_cstr("&%s", name);
        record_root_expr(ctx, function, name, decl_specs, kind, shape,
                         address_expr, num_words_expr, scope, decl_node);
        ncc_free(address_expr);
        ncc_free(num_words_expr);
        ncc_free(name);
        return;
    }

    if (aggregate) {
        char *atomic_type = atomic_offset_type_for_specs(ctx, decl_specs,
                                                        aggregate_info);
        if (is_array) {
            expand_aggregate_array(ctx, function, name, name, aggregate,
                                   declarator, kind, scope, decl_node, 0,
                                   nullptr, atomic_type,
                                   atomic_type ? "" : nullptr);
        }
        else {
            expand_aggregate_fields(ctx, function, name, name, aggregate,
                                    kind, scope, decl_node, 0,
                                    atomic_type ? name : nullptr,
                                    atomic_type,
                                    atomic_type ? "" : nullptr);
        }
        ncc_free(atomic_type);
    }

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
    if (node_starts_in_system_header(decl)) {
        return;
    }

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

static void
scan_function_definition(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *fn)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->gc_stack_maps) {
        return;
    }

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(fn, "declarator");
    ncc_parse_tree_t *body       = ncc_xform_find_child_nt(fn,
                                                           "function_body");
    ncc_parse_tree_t *compound   = ncc_xform_find_child_nt(body,
                                                           "compound_statement");
    if (!declarator || !compound) {
        return;
    }

    if (node_starts_in_system_header(fn) || node_uses_computed_goto(fn)
        || node_mentions_manual_gc_stack_api(compound)) {
        return;
    }

    char *fname = function_name(declarator);
    if (cstr_has_prefix(fname, "n00b_gc_stack_")
        || cstr_has_prefix(fname, "n00b_thread_")
        || cstr_has_prefix(fname, "_n00b_thread_")
        || cstr_has_prefix(fname, "n00b_capture_stack_")
        || cstr_has_prefix(fname, "n00b_futex_")
        || strcmp(fname, "n00b_collect") == 0
        || strcmp(fname, "n00b_longjmp") == 0
        || strcmp(fname, "_n00b_stop_the_world") == 0
        || strcmp(fname, "_n00b_restart_the_world") == 0
        || strcmp(fname, "n00b_wait_for_stw_release") == 0
        || cstr_has_prefix(fname, "__ncc_gc_stack_")
        || cstr_has_prefix(fname, "__")
        || cstr_has_prefix(fname, "ncplane_")) {
        ncc_free(fname);
        return;
    }

    const char *nonlocal_name = nullptr;
    ncc_parse_tree_t *bad_nonlocal =
        find_unsafe_nonlocal_exit_call(compound, &nonlocal_name);
    if (bad_nonlocal) {
        gc_stack_errorf(bad_nonlocal,
                        "unsafe non-local exit call '%s' in function '%s' is "
                        "not supported with n00b GC stack maps; use "
                        "n00b_setjmp()/n00b_longjmp(), a sanctioned n00b "
                        "safepoint wrapper, compile this function without "
                        "stack maps, or use ordinary error propagation",
                        nonlocal_name ? nonlocal_name : "<unknown>", fname);
    }

    ncc_parse_tree_t *params = first_descendant_nt(declarator,
                                                   "parameter_type_list");
    if (params) {
        scan_parameter_node(ctx, fname, compound, params);
    }

    scan_compound(ctx, fname, compound);
    ncc_free(fname);
}

static void
scan_function_definitions(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    if (ncc_xform_nt_name_is(node, "function_definition")) {
        if (!node_starts_in_system_header(node)) {
            scan_function_definition(ctx, node);
        }
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        scan_function_definitions(ctx, ncc_tree_child(node, i));
    }
}

typedef enum {
    GC_FRAME_GROUP_PARAMS,
    GC_FRAME_GROUP_LOCAL,
} gc_frame_group_kind_t;

typedef struct {
    gc_frame_group_kind_t kind;
    ncc_parse_tree_t     *anchor;
    ncc_parse_tree_t     *scope;
    int                   id;
} gc_frame_group_t;

typedef struct {
    gc_frame_group_t *data;
    size_t            len;
    size_t            cap;
} gc_frame_group_list_t;

static void
gc_frame_group_list_push(gc_frame_group_list_t *list, gc_frame_group_t group)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(gc_frame_group_t));
        list->cap = new_cap;
    }

    list->data[list->len++] = group;
}

static ncc_parse_tree_t *
root_group_anchor(ncc_gc_stack_root_t *root, gc_frame_group_kind_t *kind)
{
    if (root->kind == NCC_GC_STACK_ROOT_PARAM) {
        *kind = GC_FRAME_GROUP_PARAMS;
        return root->scope;
    }

    *kind = GC_FRAME_GROUP_LOCAL;
    return declaration_block_item(root->declaration);
}

static bool
group_matches_root(gc_frame_group_t *group, ncc_gc_stack_root_t *root)
{
    gc_frame_group_kind_t kind   = GC_FRAME_GROUP_LOCAL;
    ncc_parse_tree_t     *anchor = root_group_anchor(root, &kind);

    return group->kind == kind && group->anchor == anchor;
}

static void
ensure_group_for_root(ncc_xform_ctx_t *ctx, gc_frame_group_list_t *groups,
                      ncc_gc_stack_root_t *root)
{
    gc_frame_group_kind_t kind   = GC_FRAME_GROUP_LOCAL;
    ncc_parse_tree_t     *anchor = root_group_anchor(root, &kind);

    if (!anchor) {
        gc_stack_errorf(root->declaration,
                        "GC stack-map root '%s' in function '%s' has no "
                        "supported insertion point",
                        root->name, root->function_name);
    }

    for (size_t i = 0; i < groups->len; i++) {
        if (groups->data[i].kind == kind && groups->data[i].anchor == anchor) {
            return;
        }
    }

    gc_frame_group_list_push(groups, (gc_frame_group_t){
        .kind   = kind,
        .anchor = anchor,
        .scope  = root->scope,
        .id     = ctx->unique_id++,
    });
}

static size_t
count_group_roots(gc_frame_group_t *group, ncc_gc_stack_root_t *roots)
{
    size_t count = 0;

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (group_matches_root(group, root)) {
            count++;
        }
    }

    return count;
}

static char *
c_string_literal(const char *text)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_putc(buf, '"');

    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        switch (*p) {
        case '\\':
            ncc_buffer_puts(buf, "\\\\");
            break;
        case '"':
            ncc_buffer_puts(buf, "\\\"");
            break;
        case '\n':
            ncc_buffer_puts(buf, "\\n");
            break;
        case '\r':
            ncc_buffer_puts(buf, "\\r");
            break;
        case '\t':
            ncc_buffer_puts(buf, "\\t");
            break;
        default:
            if (*p < 0x20 || *p == 0x7f) {
                ncc_buffer_printf(buf, "\\x%02x", (unsigned)*p);
            }
            else {
                ncc_buffer_putc(buf, (char)*p);
            }
            break;
        }
    }

    ncc_buffer_putc(buf, '"');
    return ncc_buffer_take(buf);
}

static void
append_slot_num_words(ncc_buffer_t *buf, ncc_gc_stack_root_t *root)
{
    if (root->num_words_expr) {
        ncc_buffer_puts(buf, root->num_words_expr);
        return;
    }

    if (root->shape == NCC_GC_STACK_ROOT_AGGREGATE) {
        ncc_buffer_printf(buf,
                          "((sizeof(%s)+sizeof(void *)-1)/sizeof(void *))",
                          root->name);
        return;
    }

    if (root->shape == NCC_GC_STACK_ROOT_POINTER_ARRAY
        && root->num_words == 0) {
        ncc_buffer_printf(buf, "(sizeof(%s)/sizeof(void *))", root->name);
        return;
    }

    ncc_buffer_printf(buf, "%llu", (unsigned long long)root->num_words);
}

static bool
group_has_runtime_size_root(gc_frame_group_t *group,
                            ncc_gc_stack_root_t *roots)
{
    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (group_matches_root(group, root) && root->num_words_runtime) {
            return true;
        }
    }
    return false;
}

static char *
build_frame_source(gc_frame_group_t *group, ncc_gc_stack_root_t *roots)
{
    size_t        count    = count_group_roots(group, roots);
    ncc_buffer_t *buf      = ncc_buffer_empty();
    const char   *function = "<unknown>";
    uint32_t      line     = 0;

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (group_matches_root(group, root)) {
            function = root->function_name;
            line     = root->line;
            break;
        }
    }

    char *function_lit = c_string_literal(function);

    // If any root in this group has a runtime-determined num_words (D-033:
    // VLAs of pointer-bearing aggregates), the slot table and map cannot
    // be `static const` because a static initializer can't reference a
    // VLA's sizeof.  Emit them as block-scope locals in that case.  The
    // lifetimes are still bounded by the cleanup attribute on the frame.
    bool runtime_sized = group_has_runtime_size_root(group, roots);
    const char *slot_storage = runtime_sized ? "" : "static const ";
    const char *map_storage  = runtime_sized ? "" : "static const ";

    ncc_buffer_printf(buf,
                      "%sn00b_gc_stack_slot_t "
                      "__ncc_gc_slots_%d[] = {",
                      slot_storage, group->id);

    size_t root_index = 0;
    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (!group_matches_root(group, root)) {
            continue;
        }

        ncc_buffer_printf(buf, "{.root_index=%zu,.num_words=", root_index);
        append_slot_num_words(buf, root);
        ncc_buffer_puts(buf, "},");
        root_index++;
    }

    ncc_buffer_puts(buf, "};");
    ncc_buffer_printf(buf,
                      "%sn00b_gc_stack_map_t __ncc_gc_map_%d="
                      "{.num_roots=%zu,.num_slots=%zu,"
                      ".slots=__ncc_gc_slots_%d,"
                      ".function_name=%s,.file_name=__FILE__,.line=%u};",
                      map_storage, group->id, count, count, group->id,
                      function_lit, line);
    ncc_buffer_printf(buf, "void *__ncc_gc_roots_%d[]={", group->id);

    for (ncc_gc_stack_root_t *root = roots; root; root = root->next) {
        if (!group_matches_root(group, root)) {
            continue;
        }

        ncc_buffer_printf(buf, "(void *)%s,", root->address_expr);
    }

    ncc_buffer_puts(buf, "};");
    ncc_buffer_printf(buf,
                      "n00b_gc_stack_frame_t __ncc_gc_frame_%d "
                      "__attribute__((cleanup(n00b_gc_stack_pop)));",
                      group->id);
    ncc_buffer_printf(buf,
                      "n00b_gc_stack_push(&__ncc_gc_frame_%d,"
                      "&__ncc_gc_map_%d,__ncc_gc_roots_%d);",
                      group->id, group->id, group->id);

    ncc_free(function_lit);
    return ncc_buffer_take(buf);
}

static parse_tree_list_t
parse_generated_block_items(ncc_xform_ctx_t *ctx, const char *src)
{
    parse_tree_list_t result = {0};
    ncc_parse_tree_t *tree = ncc_xform_parse_source(ctx->grammar,
                                                    "block_item_list", src,
                                                    "xform_gc_stack_maps");
    if (!tree) {
        fprintf(stderr,
                "ncc: error: failed to parse generated GC stack-map block:\n"
                "%s\n",
                src);
        exit(1);
    }

    collect_nt_children(tree, "block_item", &result);
    if (result.len == 0 && ncc_xform_nt_name_is(tree, "block_item")) {
        parse_tree_list_push(&result, tree);
    }

    for (size_t i = 0; i < result.len; i++) {
        add_generated_spacing(result.data[i], i == 0);
    }

    return result;
}

static void
insert_block_items_at(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *compound,
                      size_t index,
                      parse_tree_list_t *items)
{
    ncc_parse_tree_t *bil = ncc_xform_find_child_nt(compound,
                                                    "block_item_list");

    if (!bil) {
        ncc_parse_tree_t **children = ncc_alloc_size(
            items->len, sizeof(ncc_parse_tree_t *));
        for (size_t i = 0; i < items->len; i++) {
            children[i] = items->data[i];
        }

        bil = ncc_xform_make_node_with_children(ctx->grammar,
                                                "block_item_list", 0,
                                                children, items->len);
        ncc_free(children);

        size_t nc = ncc_tree_num_children(compound);
        ncc_xform_insert_child(compound, nc > 0 ? nc - 1 : 0, bil);
        return;
    }

    for (size_t i = 0; i < items->len; i++) {
        ncc_xform_insert_child(bil, index + i, items->data[i]);
    }
}

static void
insert_frame_group(ncc_xform_ctx_t *ctx, gc_frame_group_t *group,
                   ncc_gc_stack_root_t *roots)
{
    char *src = build_frame_source(group, roots);
    parse_tree_list_t items = parse_generated_block_items(ctx, src);
    ncc_free(src);

    if (group->kind == GC_FRAME_GROUP_PARAMS) {
        insert_block_items_at(ctx, group->anchor, 0, &items);
        ncc_free(items.data);
        return;
    }

    ncc_nt_node_t    pn        = ncc_tree_node_value(group->anchor);
    ncc_parse_tree_t *container = (ncc_parse_tree_t *)pn.parent;
    size_t            anchor_ix = 0;

    if (!container || !child_index(container, group->anchor, &anchor_ix)) {
        gc_stack_errorf(group->anchor,
                        "GC stack-map frame insertion failed: local root "
                        "declaration is not in a block item list");
    }

    for (size_t i = 0; i < items.len; i++) {
        ncc_xform_insert_child(container, anchor_ix + 1 + i, items.data[i]);
    }

    ncc_free(items.data);
}

static ncc_parse_tree_t *
xform_gc_stack_translation_unit(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (!data || !data->gc_stack_maps) {
        return nullptr;
    }

    collect_gc_type_info(ctx, tu);
    scan_function_definitions(ctx, tu);

    if (!data->gc_stack_roots) {
        return nullptr;
    }

    gc_frame_group_list_t groups = {0};
    for (ncc_gc_stack_root_t *root = data->gc_stack_roots; root;
         root = root->next) {
        ensure_group_for_root(ctx, &groups, root);
    }

    for (size_t i = 0; i < groups.len; i++) {
        insert_frame_group(ctx, &groups.data[i], data->gc_stack_roots);
    }

    ncc_free(groups.data);
    (void)tu;
    return nullptr;
}

void
ncc_register_gc_stack_maps_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "translation_unit",
                       xform_gc_stack_translation_unit,
                       "gc_stack_maps_emit");
}
