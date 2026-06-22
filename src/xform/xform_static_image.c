// xform_static_image.c -- File-scope b"..." buffer literal lowering.

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

// WP-011 Phase 5f: vendored XXH3_128bits so the standalone buffer
// literal emission path (`static n00b_buffer_t *p = b"...";` and
// related top-level declarations) can precompute the descriptor's
// `.cached_hash` slot at build time.  Mirrors `n00b_buffer_hash` in
// libn00b's `src/core/hash.c:145-153`:
//
//   if (!b || !b->byte_len) return n00b_hash_word(0ULL);
//   return n00b_xxh_convert(XXH3_128bits(b->data, b->byte_len));
//
// Same algorithm-match contract Phase 3c.ii.b (`compute_buffer_key_hash`
// in xform_array_literal.c) and Phase 5d (`format_rstr_cached_hash_expr`
// in xform_rstr.c) follow.  Empty payloads keep both halves at zero —
// the runtime falls back to `n00b_hash_word(0ULL)` for empty buffers,
// which we cannot reproduce at ncc compile time without depending on
// libn00b's `n00b_word_t` layout.
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "vendor/xxhash.h"

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

typedef struct {
    unsigned char *bytes;
    size_t         byte_len;
    size_t         storage_len;
} buffer_static_payload_t;

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
                            "buffer literal transform could not find insertion "
                            "context");
    }

    ncc_parse_tree_t *tree = ncc_xform_parse_source(ctx->grammar, nt_name, src,
                                                    "xform_static_image");
    if (!tree) {
        fprintf(stderr,
                "ncc: error: failed to parse generated buffer literal "
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
    return name && strcmp(name, "__ncc_buflit") == 0;
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
    (void)call;
    return "b\"...\" buffer literal";
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
                            "b\"...\" buffer literal requires static "
                            "initializer arguments");
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
                                    "b\"...\" buffer literal keyword "
                                    "arguments must have the form "
                                    ".name = literal");
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
                                "b\"...\" buffer literal arguments must be "
                                "string, integer, or boolean literals; got "
                                "'%s'",
                                bad_text);
        }
        arg_list_push(&result, item);
    }

    ncc_free(args);
    return result;
}

// Inject `cached_hash_lo`/`cached_hash_hi` integer args into the
// static-init arg list for the standalone buffer literal case
// (`static n00b_buffer_t *p = b"...";`).  The single positional bytes
// arg the prescan emits for `__ncc_buflit("...")` carries the payload;
// we hash it with the same XXH3_128bits sequence `n00b_buffer_hash`
// uses at runtime, then append the two halves as named integer args for
// the migrated generalized static-init builder.
//
// Empty buffers (zero-length payload) skip the injection so the
// descriptor's `.cached_hash` slot stays at zero — `n00b_buffer_hash`
// falls back to `n00b_hash_word(0ULL)` on empty input and we cannot
// reproduce that value at ncc compile time without dragging in
// libn00b's `n00b_word_t` layout.  Empty `b""` literals therefore
// keep the pre-Phase-5f behaviour (cached_hash unset; runtime
// recompute lands on the vtable's zero-length branch).  This matches
// the empty r-string caveat documented in Phase 5d's
// `format_rstr_cached_hash_expr`.
static void
inject_buffer_cached_hash_args(static_image_arg_list_t *args)
{
    // Locate the buffer's payload bytes.  The prescan emits a single
    // positional bytes arg for `b"..."` -> `__ncc_buflit("...")`; we
    // match that case strictly to avoid accidentally hashing some
    // other byte-shaped kwarg in a future extension.
    const char *payload = nullptr;
    size_t      payload_len = 0;
    for (size_t i = 0; i < args->len; i++) {
        const static_image_arg_t *arg = &args->data[i];
        if (arg->kind != STATIC_IMAGE_ARG_BYTES) {
            continue;
        }
        // The prescan emits a single positional bytes arg.  Skip
        // anything keyword-tagged (defensive — kwargs for raw bytes
        // would land on the production helper's `.raw` / `.hex`
        // alias path, but the standalone `b"..."` literal in user
        // source always goes through positional).
        if (arg->name != nullptr) {
            continue;
        }
        payload = arg->bytes;
        payload_len = arg->byte_len;
        break;
    }

    if (!payload || payload_len == 0) {
        return;
    }

    XXH128_hash_t h = XXH3_128bits(payload, payload_len);
    // The buffer parser expects signed integer text for these cached-hash
    // arguments; cast through int64_t so the bit pattern survives sign
    // reinterpretation.
    int64_t lo_signed = (int64_t)(uint64_t)h.low64;
    int64_t hi_signed = (int64_t)(uint64_t)h.high64;

    char lo_text[32];
    char hi_text[32];
    snprintf(lo_text, sizeof(lo_text), "%lld", (long long)lo_signed);
    snprintf(hi_text, sizeof(hi_text), "%lld", (long long)hi_signed);

    static_image_arg_t lo_arg = {
        .name        = copy_cstr("cached_hash_lo"),
        .kind        = STATIC_IMAGE_ARG_INT,
        .scalar_text = copy_cstr(lo_text),
    };
    static_image_arg_t hi_arg = {
        .name        = copy_cstr("cached_hash_hi"),
        .kind        = STATIC_IMAGE_ARG_INT,
        .scalar_text = copy_cstr(hi_text),
    };
    arg_list_push(args, lo_arg);
    arg_list_push(args, hi_arg);
}

static bool
arg_name_is(const static_image_arg_t *arg, const char *name)
{
    return arg && arg->name && strcmp(arg->name, name) == 0;
}

static size_t
buffer_static_capacity(size_t len)
{
    if (len == 0) {
        return 16;
    }

    size_t cap = 1;
    while (cap < len) {
        cap <<= 1;
    }
    return cap;
}

static int
hex_digit_value(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static unsigned char *
decode_buffer_hex_arg(ncc_parse_tree_t *call,
                      const static_image_arg_t *arg,
                      size_t *out_len)
{
    if ((arg->byte_len % 2u) != 0) {
        static_image_errorf(call,
                            "migrated n00b_buffer_t static-init .hex "
                            "requires an even number of hex digits");
    }

    size_t len = arg->byte_len / 2u;
    unsigned char *data = ncc_alloc_array(unsigned char, len ? len : 1);
    for (size_t i = 0; i < len; i++) {
        int hi = hex_digit_value((unsigned char)arg->bytes[i * 2u]);
        int lo = hex_digit_value((unsigned char)arg->bytes[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            ncc_free(data);
            static_image_errorf(call,
                                "migrated n00b_buffer_t static-init .hex "
                                "contains a non-hex digit");
        }
        data[i] = (unsigned char)((hi << 4) | lo);
    }

    *out_len = len;
    return data;
}

static void
buffer_static_init_payload(ncc_parse_tree_t *call,
                           const static_image_arg_list_t *args,
                           buffer_static_payload_t *out)
{
    const unsigned char *payload = nullptr;
    unsigned char       *decoded_hex = nullptr;
    size_t               payload_len = 0;
    bool                 have_payload = false;
    bool                 have_length = false;
    size_t               length = 0;

    for (size_t i = 0; i < args->len; i++) {
        const static_image_arg_t *arg = &args->data[i];
        bool positional = arg->name == nullptr;

        if ((positional || arg_name_is(arg, "raw"))
            && arg->kind == STATIC_IMAGE_ARG_BYTES) {
            if (have_payload) {
                ncc_free(decoded_hex);
                static_image_errorf(call,
                                    "migrated n00b_buffer_t static-init "
                                    "accepts only one raw or hex payload");
            }
            payload = (const unsigned char *)arg->bytes;
            payload_len = arg->byte_len;
            have_payload = true;
            continue;
        }

        if (arg_name_is(arg, "hex") && arg->kind == STATIC_IMAGE_ARG_BYTES) {
            if (have_payload) {
                ncc_free(decoded_hex);
                static_image_errorf(call,
                                    "migrated n00b_buffer_t static-init "
                                    "accepts only one raw or hex payload");
            }
            decoded_hex = decode_buffer_hex_arg(call, arg, &payload_len);
            payload = decoded_hex;
            have_payload = true;
            continue;
        }

        if (arg_name_is(arg, "length") && arg->kind == STATIC_IMAGE_ARG_INT) {
            if (arg->scalar_text[0] == '-') {
                ncc_free(decoded_hex);
                static_image_errorf(call,
                                    "migrated n00b_buffer_t static-init "
                                    ".length must be non-negative");
            }
            char *end = nullptr;
            unsigned long long parsed = strtoull(arg->scalar_text, &end, 10);
            if (!end || *end != '\0') {
                ncc_free(decoded_hex);
                static_image_errorf(call,
                                    "migrated n00b_buffer_t static-init "
                                    ".length must be an integer literal");
            }
            length = (size_t)parsed;
            have_length = true;
            continue;
        }

        if (arg_name_is(arg, "no_lock") && arg->kind == STATIC_IMAGE_ARG_BOOL) {
            continue;
        }

        if ((arg_name_is(arg, "cached_hash_lo")
             || arg_name_is(arg, "cached_hash_hi"))
            && arg->kind == STATIC_IMAGE_ARG_INT) {
            continue;
        }

        ncc_free(decoded_hex);
        static_image_errorf(call,
                            "migrated n00b_buffer_t static-init currently "
                            "supports only b\"...\", .raw, .hex, .length, "
                            "and .no_lock arguments");
    }

    if (!have_payload) {
        payload_len = have_length ? length : 0;
    }
    else if (have_length && length != payload_len) {
        ncc_free(decoded_hex);
        static_image_errorf(call,
                            "migrated n00b_buffer_t static-init .length "
                            "must match the raw/hex payload length");
    }

    size_t storage_len = buffer_static_capacity(payload_len);
    unsigned char *bytes = ncc_alloc_array(unsigned char, storage_len);
    if (payload && payload_len) {
        memcpy(bytes, payload, payload_len);
    }

    ncc_free(decoded_hex);
    *out = (buffer_static_payload_t){
        .bytes       = bytes,
        .byte_len    = payload_len,
        .storage_len = storage_len,
    };
}

static void
append_c_byte_array(ncc_buffer_t *buf, const unsigned char *payload,
                    size_t payload_len)
{
    if (payload_len == 0) {
        ncc_buffer_puts(buf, "0");
        return;
    }

    for (size_t i = 0; i < payload_len; i++) {
        if (i > 0) {
            ncc_buffer_puts(buf, ",");
        }
        ncc_buffer_printf(buf, "0x%02x", (unsigned)payload[i]);
    }
}

static char *
buffer_static_init_runtime_decls(const char *prefix,
                                 const buffer_static_payload_t *payload)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    char *type_hash = ncc_static_object_typehash_expr("n00b_buffer_t*");
    char *payload_type_hash = ncc_static_object_typehash_expr("char*");
    XXH128_hash_t h = {0};
    if (payload->byte_len > 0) {
        h = XXH3_128bits(payload->bytes, payload->byte_len);
    }

	    ncc_buffer_printf(buf,
	                      "# line 1 \"ncc-generated-buffer-static-init-%s.c\"\n"
	                      "#ifndef N00B_BUF_F_BORROWED\n"
	                      "#define N00B_BUF_F_BORROWED (1 << 1)\n"
	                      "#endif\n"
	                      "static const unsigned char %s_payload[] = {",
	                      prefix,
	                      prefix);
	    append_c_byte_array(buf, payload->bytes, payload->storage_len);
	    ncc_buffer_printf(buf,
	                      "};\n"
	                      "extern n00b_buffer_t *n00b_crt_alloc_static_buffer("
	                      "unsigned long long, unsigned long long, "
	                      "unsigned long long);\n"
	                      "extern void *n00b_crt_alloc_static_payload("
	                      "unsigned long long, unsigned long long, "
	                      "unsigned long long, unsigned int, "
	                      "n00b_gc_scan_cb_t, void *);\n"
	                      "static n00b_buffer_t *%s_make(void)\n"
	                      "{\n"
	                      "    n00b_buffer_t *buf = "
	                      "n00b_crt_alloc_static_buffer("
	                      "%s, 0x%016llxULL, 0x%016llxULL);\n"
	                      "    char *data = n00b_crt_alloc_static_payload("
	                      "%zuULL, (unsigned long long)sizeof(char), %s, "
	                      "(unsigned int)N00B_GC_SCAN_KIND_NONE, "
	                      "nullptr, nullptr);\n"
	                      "    if (!buf || !data) { __builtin_trap(); }\n"
	                      "    __builtin_memcpy(data, %s_payload, %zuULL);\n"
	                      "    buf->data = data;\n"
	                      "    buf->byte_len = %zuULL;\n"
	                      "    buf->alloc_len = %zuULL;\n"
	                      "    buf->lock = nullptr;\n"
	                      "    buf->allocator = nullptr;\n"
	                      "    buf->flags = N00B_BUF_F_BORROWED;\n"
	                      "    buf->scan_kind = N00B_GC_SCAN_KIND_NONE;\n"
	                      "    buf->scan_cb = nullptr;\n"
	                      "    buf->scan_user = nullptr;\n"
	                      "    return buf;\n"
	                      "}\n"
	                      "%s",
	                      prefix, type_hash,
	                      (unsigned long long)h.high64,
	                      (unsigned long long)h.low64,
	                      payload->storage_len,
	                      payload_type_hash,
	                      prefix,
	                      payload->storage_len,
	                      payload->byte_len,
	                      payload->storage_len,
	                      "# line 1 \"ncc-generated-buffer-static-init-end.c\"\n");

    ncc_free(type_hash);
    ncc_free(payload_type_hash);
    return ncc_buffer_take(buf);
}

static bool
target_is_static_init_buffer(ncc_xform_ctx_t *ctx,
                             ncc_parse_tree_t *init_decl,
                             const char *type_name)
{
    char *compact = compact_type(type_name);
    bool  is_buffer = strcmp(compact, "n00b_buffer_t") == 0;
    ncc_free(compact);
    if (!is_buffer) {
        return false;
    }

    ncc_parse_tree_t *declarator =
        ncc_xform_find_child_nt(init_decl, "declarator");
    char *name = declarator_name(declarator);
    ncc_sym_entry_t *entry = lookup_static_init_entry(ctx, name);
    ncc_free(name);
    return entry != nullptr;
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
        static_image_errorf(call,
                            "%s initializer must target n00b_buffer_t *",
                            syntax_name);
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
                            "block-scope buffer literal references",
                            syntax_name);
    }

    int id = ctx->unique_id++;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "__ncc_buflit_%d", id);

    static_image_arg_list_t args = collect_static_image_args(call);

    // Precompute the payload's XXH3_128bits so every standalone
    // `b"..."` literal emission (file-scope declarations like
    // `static const n00b_buffer_t *p = b"...";`) lands a populated
    // `.cached_hash` slot in the generated static-init descriptor.
    // Together with `lower_buffer_literal_ref`'s update
    // (xform_array_literal.c), every buffer literal in ncc-compiled
    // source carries the same XXH3 the runtime would compute, so
    // cross-occurrence `n00b_hash(buffer_ptr)` results are bit-identical.
    //
    inject_buffer_cached_hash_args(&args);

    if (target_is_static_init_buffer(ctx, init_decl, type_name)) {
        if (!readonly) {
            free_arg_list(&args);
            static_image_errorf(call,
                                "migrated n00b_buffer_t static-init "
                                "requires a const target");
        }

        buffer_static_payload_t payload = {};
        buffer_static_init_payload(call, &args, &payload);

        char *decls = buffer_static_init_runtime_decls(prefix, &payload);
        insert_generated_decl(ctx, decl, decls);

        ncc_buffer_t *expr = ncc_buffer_empty();
        ncc_buffer_printf(expr, "%s_make()", prefix);
        char *expr_name = ncc_buffer_take(expr);
        ncc_parse_tree_t *replacement = ncc_xform_parse_source(
            ctx->grammar, "initializer", expr_name, "xform_static_image");
        if (!replacement) {
            fprintf(stderr,
                    "ncc: error: failed to parse generated buffer "
                    "static-init initializer:\n%s\n",
                    expr_name);
            exit(1);
        }

        replace_initializer(init_decl, replacement);
        ncc_free(type_name);
        ncc_free(decls);
        ncc_free(expr_name);
        ncc_free(payload.bytes);
        free_arg_list(&args);
        return;
    }

    ncc_free(type_name);
    free_arg_list(&args);
    static_image_errorf(call, "%s is only supported for migrated "
                              "n00b_buffer_t static-init targets",
                        syntax_name);
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
