// xform_static_image.c -- Constructor/static-image literal lowering.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"
#include "xform/xform_helpers.h"
#include "xform/xform_static_object.h"
#include "xform/xform_type_layout.h"
#include "xform/xform_data.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ncc_parse_tree_t **data;
    size_t             len;
    size_t             cap;
} static_image_node_list_t;

typedef enum {
    STATIC_IMAGE_ARG_BYTES,
    STATIC_IMAGE_ARG_INT,
    STATIC_IMAGE_ARG_BOOL,
} static_image_arg_kind_t;

typedef struct {
    char                   *name;
    static_image_arg_kind_t kind;
    char                   *bytes;
    size_t                  byte_len;
    char                   *scalar_text;
    ncc_parse_tree_t       *node;
} static_image_arg_t;

typedef struct {
    static_image_arg_t *data;
    size_t              len;
    size_t              cap;
} static_image_arg_list_t;

static const char *static_image_host_endian_value(void);

static void
static_image_errorf(ncc_parse_tree_t *node, const char *fmt, ...)
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
is_group_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return pn.name.data && pn.name.data[0] == '$' && pn.name.data[1] == '$';
}

static void
node_list_push(static_image_node_list_t *list, ncc_parse_tree_t *node)
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
arg_list_push(static_image_arg_list_t *list, static_image_arg_t arg)
{
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        list->data = ncc_realloc(list->data,
                                  new_cap * sizeof(static_image_arg_t));
        list->cap = new_cap;
    }

    list->data[list->len++] = arg;
}

static void
free_arg_list(static_image_arg_list_t *list)
{
    for (size_t i = 0; i < list->len; i++) {
        ncc_free(list->data[i].name);
        ncc_free(list->data[i].bytes);
        ncc_free(list->data[i].scalar_text);
    }
    ncc_free(list->data);
    *list = (static_image_arg_list_t){0};
}

static void
collect_nt_children(ncc_parse_tree_t *node, const char *name,
                    static_image_node_list_t *out)
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
node_text(ncc_parse_tree_t *node)
{
    ncc_string_t s = ncc_xform_node_to_text(node);
    char        *r = ncc_layout_trim_copy(s.data);
    ncc_free(s.data);
    return r;
}

static char *
copy_cstr(const char *text)
{
    size_t len = strlen(text);
    char  *out = ncc_alloc_array(char, len + 1);
    memcpy(out, text, len + 1);
    return out;
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
        static_image_errorf(decl,
                            "static image transform could not find insertion "
                            "context");
    }

    ncc_parse_tree_t *tree = ncc_xform_parse_source(ctx->grammar, nt_name, src,
                                                    "xform_static_image");
    if (!tree) {
        fprintf(stderr,
                "ncc: error: failed to parse generated static image "
                "declaration:\n%s\n",
                src);
        exit(1);
    }

    static_image_node_list_t generated = {0};
    collect_nt_children(tree, item_nt, &generated);
    if (generated.len == 0 && ncc_xform_nt_name_is(tree, item_nt)) {
        node_list_push(&generated, tree);
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

    ncc_free(generated.data);
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

static const char *
callee_name(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node) || ncc_tree_num_children(node) == 0) {
        return nullptr;
    }

    ncc_parse_tree_t *callee = ncc_tree_child(node, 0);
    while (callee && !ncc_tree_is_leaf(callee)
           && ncc_tree_num_children(callee) > 0) {
        callee = ncc_tree_child(callee, 0);
    }

    return ncc_xform_leaf_text(callee);
}

static bool
is_static_image_call(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)
        || !ncc_xform_nt_name_is(node, "postfix_expression")
        || ncc_tree_num_children(node) < 3
        || !ncc_xform_leaf_text_eq(ncc_tree_child(node, 1), "(")) {
        return false;
    }

    const char *name = callee_name(node);
    return name && (strcmp(name, "ncc_static_image") == 0
                    || strcmp(name, "__ncc_buflit") == 0);
}

static bool
is_buffer_literal_call(ncc_parse_tree_t *node)
{
    const char *name = callee_name(node);
    return name && strcmp(name, "__ncc_buflit") == 0;
}

static const char *
static_image_syntax_name(ncc_parse_tree_t *call)
{
    return is_buffer_literal_call(call) ? "b\"...\" buffer literal"
                                       : "ncc_static_image()";
}

static ncc_parse_tree_t *
find_static_image_call(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (is_static_image_call(node)) {
        return node;
    }

    if (ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found = find_static_image_call(ncc_tree_child(node,
                                                                        i));
        if (found) {
            return found;
        }
    }

    return nullptr;
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
decode_string_literal_sequence(ncc_parse_tree_t *node, ncc_buffer_t *out,
                               int *string_count)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (!text) {
            return true;
        }
        if (!string_literal_leaf(text)) {
            return false;
        }
        decode_string_leaf(out, text);
        (*string_count)++;
        return true;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (!decode_string_literal_sequence(ncc_tree_child(node, i), out,
                                            string_count)) {
            return false;
        }
    }

    return true;
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

static char *
keyword_name(ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *ident = ncc_xform_find_child_nt(node, "identifier");
    const char       *text  = ncc_xform_get_first_leaf_text(ident);
    if (!text || !*text) {
        return nullptr;
    }

    return copy_cstr(text);
}

static ncc_parse_tree_t *
find_keyword_argument(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "keyword_argument")) {
        return node;
    }

    if (!is_group_node(node)
        && !ncc_xform_nt_name_is(node, "argument_expression_list")) {
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *found = find_keyword_argument(ncc_tree_child(node,
                                                                       i));
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static ncc_parse_tree_t *
keyword_rhs_candidate(ncc_parse_tree_t *node)
{
    while (node && is_group_node(node) && ncc_tree_num_children(node) == 1) {
        node = ncc_tree_child(node, 0);
    }

    if (!node || ncc_tree_is_leaf(node)) {
        return node;
    }

    if (ncc_xform_nt_name_is(node, "assignment_expression")
        || ncc_xform_nt_name_is(node, "conditional_expression")) {
        return node;
    }

    ncc_parse_tree_t *expr = ncc_xform_find_child_nt(node,
                                                     "assignment_expression");
    if (expr) {
        return expr;
    }

    expr = ncc_xform_find_child_nt(node, "conditional_expression");
    if (expr) {
        return expr;
    }

    return node;
}

static ncc_parse_tree_t *
keyword_value_expr(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    bool saw_equal = false;
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (!child) {
            continue;
        }

        if (ncc_tree_is_leaf(child)) {
            if (ncc_xform_leaf_text_eq(child, "=")) {
                saw_equal = true;
            }
            continue;
        }

        if (saw_equal) {
            return keyword_rhs_candidate(child);
        }

        ncc_parse_tree_t *found = keyword_value_expr(child);
        if (found) {
            return found;
        }
    }

    return nullptr;
}

static bool
text_is_integer_literal(const char *text)
{
    if (!text || !*text) {
        return false;
    }

    const char *p = text;
    if (*p == '+' || *p == '-') {
        p++;
    }
    if (!isdigit((unsigned char)*p)) {
        return false;
    }
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static bool
classify_static_arg(ncc_parse_tree_t *expr, static_image_arg_t *arg)
{
    ncc_buffer_t *bytes = ncc_buffer_empty();
    int string_count = 0;
    if (decode_string_literal_sequence(expr, bytes, &string_count)
        && string_count > 0) {
        arg->kind     = STATIC_IMAGE_ARG_BYTES;
        arg->byte_len = bytes->byte_len;
        arg->bytes    = ncc_buffer_take(bytes);
        return true;
    }
    ncc_buffer_free(bytes);

    char *text = node_text(expr);
    if (strcmp(text, "true") == 0 || strcmp(text, "false") == 0
        || strcmp(text, "0") == 0 || strcmp(text, "1") == 0) {
        arg->kind        = STATIC_IMAGE_ARG_BOOL;
        arg->scalar_text = text;
        return true;
    }

    if (text_is_integer_literal(text)) {
        arg->kind        = STATIC_IMAGE_ARG_INT;
        arg->scalar_text = text;
        return true;
    }

    ncc_free(text);
    return false;
}

static static_image_arg_list_t
collect_static_image_args(ncc_parse_tree_t *call)
{
    if (ncc_tree_num_children(call) < 3) {
        static_image_errorf(call,
                            "ncc_static_image() requires static initializer "
                            "arguments");
    }

    ncc_parse_tree_t *arglist = ncc_tree_child(call, 2);
    if (!arglist || ncc_xform_leaf_text_eq(arglist, ")")) {
        return (static_image_arg_list_t){0};
    }

    int nargs = 0;
    ncc_parse_tree_t **args = collect_arguments(arglist, &nargs);
    static_image_arg_list_t result = {0};

    for (int i = 0; i < nargs; i++) {
        ncc_parse_tree_t *arg_node = args[i];
        ncc_parse_tree_t *expr     = arg_node;
        char             *name     = nullptr;

        ncc_parse_tree_t *kw_node = find_keyword_argument(arg_node);
        if (kw_node) {
            name = keyword_name(kw_node);
            expr = keyword_value_expr(kw_node);
            if (!name || !expr) {
                ncc_free(name);
                ncc_free(args);
                free_arg_list(&result);
                static_image_errorf(arg_node,
                                    "ncc_static_image() keyword arguments "
                                    "must have the form .name = literal");
            }
        }

        static_image_arg_t item = {
            .name = name,
            .node = arg_node,
        };
        if (!classify_static_arg(expr, &item)) {
            char *bad_text = node_text(expr);
            ncc_free(args);
            free_arg_list(&result);
            static_image_errorf(arg_node,
                                "ncc_static_image() arguments must be string, "
                                "integer, or boolean literals for the static "
                                "initializer helper; got '%s'",
                                bad_text);
        }
        arg_list_push(&result, item);
    }

    ncc_free(args);
    return result;
}

static char *
build_helper_request(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *call,
                     const char *type_name, const char *prefix, bool readonly,
                     const static_image_arg_list_t *args)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    char *type_hex = hex_string(type_name, strlen(type_name));
    ncc_buffer_t *hash_type_buf = ncc_buffer_empty();
    ncc_buffer_printf(hash_type_buf, "%s*", type_name);
    char *hash_type = ncc_buffer_take(hash_type_buf);
    char *type_hash = ncc_static_object_typehash_expr(hash_type);
    const char *entry_attr = ncc_static_object_entry_attr(ctx);
    char *entry_attr_hex = hex_string(entry_attr, strlen(entry_attr));
    char *identity_namespace =
        ncc_static_object_identity_namespace(ctx, call);
    char *identity_object_key =
        ncc_static_object_identity_key(ctx,
                                       "ncc-static-image-object",
                                       call,
                                       type_hash,
                                       "1");
    char *identity_payload_key =
        ncc_static_object_identity_key(ctx,
                                       "ncc-static-image-payload",
                                       call,
                                       "0",
                                       "payload");
    char *identity_namespace_hex =
        hex_string(identity_namespace, strlen(identity_namespace));
    char *identity_object_key_hex =
        hex_string(identity_object_key, strlen(identity_object_key));
    char *identity_payload_key_hex =
        hex_string(identity_payload_key, strlen(identity_payload_key));

    ncc_buffer_printf(buf,
                      "NCC_STATIC_INIT 1\n"
                      "type_hex %s\n"
                      "type_hash %s\n"
                      "prefix %s\n"
                      "readonly %u\n"
                      "abi %zu %zu 8 %s\n"
                      "entry_attr_hex %s\n"
                      "identity_namespace_hex %s\n"
                      "identity_object_key_hex %s\n"
                      "identity_payload_key_hex %s\n"
                      "arg_count %zu\n",
                      type_hex, type_hash, prefix, readonly ? 1u : 0u,
                      sizeof(void *), sizeof(size_t),
                      static_image_host_endian_value(),
                      entry_attr_hex,
                      identity_namespace_hex,
                      identity_object_key_hex,
                      identity_payload_key_hex,
                      args->len);

    for (size_t i = 0; i < args->len; i++) {
        const static_image_arg_t *arg = &args->data[i];
        const char *name = arg->name ? arg->name : "-";
        if (arg->kind == STATIC_IMAGE_ARG_BYTES) {
            char *payload_hex = hex_string(arg->bytes, arg->byte_len);
            ncc_buffer_printf(buf, "arg %s bytes %zu %s\n", name,
                              arg->byte_len, payload_hex);
            ncc_free(payload_hex);
        }
        else if (arg->kind == STATIC_IMAGE_ARG_INT) {
            ncc_buffer_printf(buf, "arg %s int %s\n", name,
                              arg->scalar_text);
        }
        else {
            bool value = strcmp(arg->scalar_text, "true") == 0
                      || strcmp(arg->scalar_text, "1") == 0;
            ncc_buffer_printf(buf, "arg %s bool %u\n", name,
                              value ? 1u : 0u);
        }
    }

    ncc_buffer_puts(buf, "end\n");

    ncc_free(type_hex);
    ncc_free(hash_type);
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

static void
static_image_helper_error(ncc_parse_tree_t *call, const char *type_name,
                          const char *message, const char *stderr_data,
                          size_t stderr_len)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "ncc_static_image() static initializer for '%s' %s",
                      type_name, message);
    if (stderr_data && stderr_len) {
        ncc_buffer_puts(buf, ": ");
        ncc_buffer_append(buf, stderr_data, stderr_len);
    }

    char *text = ncc_buffer_take(buf);
    static_image_errorf(call, "%s", text);
}

static void
run_static_init_helper(ncc_parse_tree_t *call, const char *helper,
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
        static_image_helper_error(call, type_name, "could not be launched",
                                  proc.stderr_data, proc.stderr_len);
    }

    if (proc.exit_code != 0) {
        static_image_helper_error(call, type_name, "failed",
                                  proc.stderr_data, proc.stderr_len);
    }

    const char prefix[] = "NCC_STATIC_INIT_OK ";
    if (!proc.stdout_data
        || proc.stdout_len < sizeof(prefix) - 1
        || strncmp(proc.stdout_data, prefix, sizeof(prefix) - 1) != 0) {
        static_image_helper_error(call, type_name,
                                  "returned an invalid response",
                                  proc.stderr_data, proc.stderr_len);
    }

    char *newline = memchr(proc.stdout_data, '\n', proc.stdout_len);
    if (!newline || newline == proc.stdout_data + sizeof(prefix) - 1) {
        static_image_helper_error(call, type_name,
                                  "returned an invalid response",
                                  proc.stderr_data, proc.stderr_len);
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
target_is_readonly(ncc_parse_tree_t *decl_specs)
{
    return contains_leaf_text(decl_specs, "const")
        || contains_leaf_text(decl_specs, "__const")
        || contains_leaf_text(decl_specs, "__const__");
}

static bool
declaration_is_block_scope(ncc_parse_tree_t *decl)
{
    return ncc_xform_find_ancestor(decl, "block_item") != nullptr;
}

static char *
static_image_strip_decl_keywords(char *raw)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (const char *p = raw; p && *p;) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        const char *start = p;
        if (isalpha((unsigned char)*p) || *p == '_') {
            p++;
            while (isalnum((unsigned char)*p) || *p == '_') {
                p++;
            }

            size_t len = (size_t)(p - start);
            if ((len == 5 && strncmp(start, "const", len) == 0)
                || (len == 8 && strncmp(start, "volatile", len) == 0)
                || (len == 6 && strncmp(start, "static", len) == 0)
                || (len == 6 && strncmp(start, "extern", len) == 0)
                || (len == 8 && strncmp(start, "register", len) == 0)
                || (len == 7 && strncmp(start, "typedef", len) == 0)
                || (len == 6 && strncmp(start, "inline", len) == 0)
                || (len == 10 && strncmp(start, "__const__", len) == 0)
                || (len == 7 && strncmp(start, "__const", len) == 0)
                || (len == 12 && strncmp(start, "__volatile__", len) == 0)
                || (len == 10 && strncmp(start, "__volatile", len) == 0)) {
                continue;
            }
            if (buf->byte_len > 0) {
                ncc_buffer_putc(buf, ' ');
            }
            ncc_buffer_append(buf, start, len);
            continue;
        }

        if (buf->byte_len > 0) {
            ncc_buffer_putc(buf, ' ');
        }
        ncc_buffer_putc(buf, *p++);
    }

    ncc_free(raw);
    return ncc_buffer_take(buf);
}

static char *
static_image_base_type(ncc_parse_tree_t *decl_specs)
{
    char *raw    = ncc_xform_collect_base_type(decl_specs);
    char *result = static_image_strip_decl_keywords(raw);

    if (!result[0] || strcmp(result, "int") == 0) {
        ncc_free(result);
        raw    = node_text(decl_specs);
        result = static_image_strip_decl_keywords(raw);
    }

    return result;
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
lower_static_image_initializer(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                               ncc_parse_tree_t *decl_specs,
                               ncc_parse_tree_t *init_decl,
                               ncc_parse_tree_t *init,
                               ncc_parse_tree_t *call)
{
    (void)init;

    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(init_decl,
                                                           "declarator");
    int ptr_depth = ncc_layout_pointer_depth_in_declarator(declarator);
    const char *syntax_name = static_image_syntax_name(call);
    if (ptr_depth == 0) {
        if (is_buffer_literal_call(call)) {
            static_image_errorf(call,
                                "%s initializer must target n00b_buffer_t *",
                                syntax_name);
        }
        else {
            static_image_errorf(call,
                                "%s initializer must target a pointer to a "
                                "static-image type",
                                syntax_name);
        }
    }

    char *type_name = static_image_base_type(decl_specs);
    if (is_buffer_literal_call(call)) {
        char *compact = compact_type(type_name);
        bool is_buffer = strcmp(compact, "n00b_buffer_t") == 0;
        ncc_free(compact);
        if (!is_buffer) {
            static_image_errorf(call,
                                "%s initializer must target n00b_buffer_t *; "
                                "found '%s *'",
                                syntax_name, type_name);
        }
    }

    bool   readonly    = target_is_readonly(decl_specs);
    if (!readonly && declaration_is_block_scope(decl)) {
        static_image_errorf(call,
                            "mutable %s initializers must be "
                            "declared at file scope; use a const target for "
                            "block-scope static image references",
                            syntax_name);
    }

    int id = ctx->unique_id++;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__ncc_static_image_%d", id);

    static_image_arg_list_t args = collect_static_image_args(call);
    const char *helper = ncc_xform_get_data(ctx)->static_init_helper;
    if (!helper || !*helper) {
        free_arg_list(&args);
        static_image_errorf(call,
                            "%s for '%s' requires "
                            "--ncc-static-init-helper=PATH",
                            syntax_name, type_name);
    }

    char *request = build_helper_request(ctx, call, type_name, prefix, readonly,
                                         &args);
    char *expr_name = nullptr;
    char *decls     = nullptr;
    run_static_init_helper(call, helper, type_name, request, &expr_name,
                           &decls);
    insert_generated_decl(ctx, decl, decls);

    ncc_parse_tree_t *replacement = ncc_xform_parse_source(
        ctx->grammar, "initializer", expr_name, "xform_static_image");
    if (!replacement) {
        fprintf(stderr,
                "ncc: error: failed to parse generated static image "
                "initializer:\n%s\n",
                expr_name);
        exit(1);
    }

    replace_initializer(init_decl, replacement);
    ncc_free(type_name);
    ncc_free(request);
    ncc_free(expr_name);
    ncc_free(decls);
    free_arg_list(&args);
}

static ncc_parse_tree_t *
xform_static_image_decl(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                        ncc_xform_control_t *control)
{
    (void)control;

    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        decl, "declaration_specifiers");
    ncc_parse_tree_t *list = ncc_xform_find_child_nt(decl,
                                                     "init_declarator_list");
    if (!decl_specs || !list) {
        return nullptr;
    }

    static_image_node_list_t init_decls = {0};
    collect_nt_children(list, "init_declarator", &init_decls);

    for (size_t i = 0; i < init_decls.len; i++) {
        ncc_parse_tree_t *init = ncc_xform_find_child_nt(init_decls.data[i],
                                                         "initializer");
        ncc_parse_tree_t *call = find_static_image_call(init);
        if (!call) {
            continue;
        }

        char *init_src = node_text(init);
        char *call_src = node_text(call);
        if (strcmp(init_src, call_src) != 0) {
            static_image_errorf(call,
                                "%s must be used as the whole initializer "
                                "expression",
                                static_image_syntax_name(call));
        }
        ncc_free(init_src);
        ncc_free(call_src);

        lower_static_image_initializer(ctx, decl, decl_specs,
                                       init_decls.data[i], init, call);
    }

    ncc_free(init_decls.data);
    return nullptr;
}

void
ncc_register_static_image_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "declaration", xform_static_image_decl,
                           "static_image");
}
