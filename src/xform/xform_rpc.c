// xform_rpc.c — Transform: `@rpc("svc/method")` function annotation.
//
// Recognizes `@rpc(...)` on a function declaration or definition.
// Validates the method string, strips the clause so plain clang
// accepts the source, and emits the dispatcher + constructor +
// client-stub trio for the matching unary / streaming shape.
//
// Operates pre-order on "translation_unit". We flatten the TU's
// `$$group_*` wrapper, walk the external_declarations, strip + maybe
// synthesize for each annotated one, then rebuild the TU with the
// original (now-clause-free) decls followed by the synthesized
// dispatcher / ctor / stub.
//
// Must run FIRST in the xform sequence so the synthesized bodies
// flow through generic_struct / typeid / option / kargs_vargs after
// the synthesis.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_rpc.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Method-string validator
//
// Regex equivalent (per spec §5.3):
//   ^[a-zA-Z_][a-zA-Z0-9_.]*\.[A-Z][a-zA-Z0-9]*\/[A-Z][a-zA-Z0-9]*$
//
// Hand-rolled to avoid pulling in a regex dependency for one
// validation site.
// ============================================================================

static bool
is_lower_alpha_digit_under(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool
is_alpha_digit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
}

static bool
validate_method_string(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    const char *slash = strchr(s, '/');

    if (!slash || strchr(slash + 1, '/')) {
        return false;
    }

    // Method (after slash): [A-Z][a-zA-Z0-9]*
    const char *m = slash + 1;

    if (*m < 'A' || *m > 'Z') {
        return false;
    }

    for (m++; *m; m++) {
        if (!is_alpha_digit(*m)) {
            return false;
        }
    }

    // Service path (before slash): identifier chars / dots, at least
    // one dot, and the segment after the last dot must start with an
    // uppercase letter.
    size_t pre_len = (size_t)(slash - s);

    if (pre_len == 0) {
        return false;
    }

    if (!(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z')
          || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }

    const char *last_dot = nullptr;

    for (size_t i = 0; i < pre_len; i++) {
        char c = s[i];

        if (c == '.') {
            last_dot = s + i;
        }
        else if (!is_lower_alpha_digit_under(c)) {
            return false;
        }
    }

    if (!last_dot) {
        return false;
    }

    const char *svc = last_dot + 1;

    if (svc >= slash) {
        return false;
    }

    if (*svc < 'A' || *svc > 'Z') {
        return false;
    }

    for (svc++; svc < slash; svc++) {
        if (!is_alpha_digit(*svc)) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Extract the method string from an <rpc_clause> subtree.
//
// Grammar: <rpc_clause> ::= %"@" %"rpc" %"(" <string_literal> %")"
// <string_literal> ::= %STRING+   — each STRING leaf carries its own
// surrounding quotes; for adjacent string literals we concatenate
// their contents.
// ============================================================================

static char *
extract_method_string(ncc_parse_tree_t *rpc_clause)
{
    ncc_parse_tree_t *str_lit
        = ncc_xform_find_child_nt(rpc_clause, "string_literal");

    if (!str_lit) {
        return nullptr;
    }

    ncc_string_t text = ncc_xform_node_to_text(str_lit);

    if (!text.data) {
        return nullptr;
    }

    // Strip outer quotes (and any whitespace) for each STRING piece.
    // Walks the verbatim text and copies bytes that lie strictly
    // between matched quote pairs.
    char *out  = ncc_alloc_size(1, text.u8_bytes + 1);
    size_t  oi = 0;
    bool    in = false;

    for (size_t i = 0; i < text.u8_bytes; i++) {
        char c = text.data[i];

        if (c == '"') {
            in = !in;
            continue;
        }

        if (!in) {
            continue;
        }

        if (c == '\\' && i + 1 < text.u8_bytes) {
            // Preserve backslash escapes verbatim — the method
            // grammar doesn't allow them, but we'd rather hand the
            // text to the validator and produce a precise error than
            // strip them into something unrecognizable.
            out[oi++] = text.data[i++];
            out[oi++] = text.data[i];
            continue;
        }

        out[oi++] = c;
    }

    out[oi] = '\0';
    ncc_free(text.data);
    return out;
}

// ============================================================================
// Find an <rpc_clause> or <keyword_clause> child of a
// function_definition / declaration.
// ============================================================================

static ncc_parse_tree_t *
unwrap_extension_function_definition(ncc_parse_tree_t *func_def)
{
    while (func_def && ncc_xform_nt_name_is(func_def, "function_definition")
           && ncc_tree_num_children(func_def) == 2) {
        ncc_parse_tree_t *first  = ncc_tree_child(func_def, 0);
        ncc_parse_tree_t *second = ncc_tree_child(func_def, 1);

        if (!first || !ncc_tree_is_leaf(first)
            || !ncc_xform_leaf_text_eq(first, "__extension__")
            || !second || ncc_tree_is_leaf(second)
            || !ncc_xform_nt_name_is(second, "function_definition")) {
            break;
        }

        func_def = second;
    }

    return func_def;
}

static ncc_parse_tree_t *
find_child_nt_index(ncc_parse_tree_t *parent, const char *name, size_t *idx)
{
    size_t nc = ncc_tree_num_children(parent);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(parent, i);

        if (c && !ncc_tree_is_leaf(c) && ncc_xform_nt_name_is(c, name)) {
            if (idx) {
                *idx = i;
            }
            return c;
        }
    }

    return nullptr;
}

// ============================================================================
// Diagnostics
// ============================================================================

[[noreturn]] static void
rpc_diagnostic(ncc_parse_tree_t *anchor, const char *fmt, ...)
{
    uint32_t line = 0, col = 0;

    if (anchor) {
        ncc_xform_first_leaf_pos(anchor, &line, &col);
    }

    fprintf(stderr, "ncc: error: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, " (line %u, col %u)\n", line, col);
    exit(1);
}

// ============================================================================
// Function-name extraction (copy of xform_once's idiom; the helper
// itself is static there so we re-implement rather than link)
// ============================================================================

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

static size_t
find_last_top_level_paren_open(const char *text)
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
extract_func_name(ncc_parse_tree_t *declarator)
{
    ncc_string_t text = ncc_xform_node_to_text(declarator);

    if (!text.data) {
        return nullptr;
    }

    size_t paren = find_last_top_level_paren_open(text.data);
    size_t end   = (paren == (size_t)-1) ? text.u8_bytes : paren;
    char  *name  = copy_last_identifier_before(text.data, end);

    ncc_free(text.data);
    return name;
}

// ============================================================================
// Method-string splitting + sanitization
//
// Input  : "package.path.Service/Method"
// Output : svc_underscored = "package_path_Service"
//          method          = "Method"
// ============================================================================

static void
split_method(const char *method_str, char **svc_out, char **method_out)
{
    const char *slash = strchr(method_str, '/');

    // Validator already ran; both pieces exist.
    size_t pre_len = (size_t)(slash - method_str);

    char *svc = ncc_alloc_size(1, pre_len + 1);
    memcpy(svc, method_str, pre_len);
    svc[pre_len] = '\0';

    for (size_t i = 0; i < pre_len; i++) {
        if (svc[i] == '.') {
            svc[i] = '_';
        }
    }

    *svc_out    = svc;
    *method_out = ncc_string_from_cstr(slash + 1).data;
}

// ============================================================================
// Type-parameter extraction from `typeid("result", T *)` leaf text
//
// Returns the inner T (with the trailing `*` removed and whitespace
// trimmed). Returns nullptr if the pattern isn't present.
// ============================================================================

static char *
extract_typeid_inner(const char *text, const char *tag)
{
    if (!text) {
        return nullptr;
    }

    // Locate `typeid(<ws>?"<tag>"<ws>?,` allowing arbitrary
    // whitespace between tokens (the leaf-text emitter inserts
    // single spaces).
    size_t      tag_len = strlen(tag);
    const char *p       = text;
    const char *after   = nullptr;

    while ((p = strstr(p, "typeid")) != nullptr) {
        const char *q = p + 6;

        while (*q == ' ' || *q == '\t' || *q == '\n') {
            q++;
        }

        if (*q != '(') {
            p++;
            continue;
        }

        q++;

        while (*q == ' ' || *q == '\t' || *q == '\n') {
            q++;
        }

        if (*q != '"') {
            p++;
            continue;
        }

        q++;

        if (strncmp(q, tag, tag_len) == 0 && q[tag_len] == '"') {
            q += tag_len + 1;

            while (*q == ' ' || *q == '\t' || *q == '\n') {
                q++;
            }

            if (*q == ',') {
                after = q + 1;
                break;
            }
        }

        p++;
    }

    if (!after) {
        return nullptr;
    }

    p = after;

    while (*p == ' ' || *p == '\t' || *p == '\n') {
        p++;
    }

    // Scan to matching close-paren, ignoring nested parens.
    int         depth = 1;
    const char *start = p;

    while (*p && depth > 0) {
        if (*p == '(') {
            depth++;
        }
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }

        p++;
    }

    if (depth != 0) {
        return nullptr;
    }

    // Trim trailing whitespace.
    const char *end = p;

    while (end > start && (end[-1] == ' ' || end[-1] == '\t'
                            || end[-1] == '\n')) {
        end--;
    }

    // Strip a trailing `*`.
    if (end > start && end[-1] == '*') {
        end--;

        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
    }

    size_t len = (size_t)(end - start);

    if (len == 0) {
        return nullptr;
    }

    char *out = ncc_alloc_size(1, len + 1);
    memcpy(out, start, len);
    out[len] = '\0';

    // Collapse internal whitespace runs to single spaces for use in
    // a generated identifier or type position. (Real users won't
    // embed weird whitespace here; this guards the synthesized
    // identifiers we paste together later.)
    char  *w     = out;
    bool   in_ws = false;

    for (char *r = out; *r; r++) {
        if (*r == ' ' || *r == '\t' || *r == '\n') {
            if (!in_ws) {
                *w++  = ' ';
                in_ws = true;
            }
        }
        else {
            *w++  = *r;
            in_ws = false;
        }
    }

    *w = '\0';

    return out;
}

// ============================================================================
// Extract the type of the first parameter (the request, in unary
// shape). The expected leaf text shape is `T *<name>` — peel off
// the trailing identifier and the `*`.
// ============================================================================

static ncc_parse_tree_t *
find_descendant_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, name)) {
        return node;
    }

    size_t nc = ncc_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *r = find_descendant_nt(ncc_tree_child(node, i),
                                                  name);
        if (r) {
            return r;
        }
    }

    return nullptr;
}

static char *
extract_first_param_type(ncc_parse_tree_t *declarator)
{
    ncc_parse_tree_t *ptl = find_descendant_nt(declarator,
                                                "parameter_type_list");

    if (!ptl) {
        return nullptr;
    }

    ncc_parse_tree_t *pl = ncc_xform_find_child_nt(ptl, "parameter_list");

    if (!pl) {
        pl = ptl;
    }

    // First parameter_declaration child.
    ncc_parse_tree_t *first_pd = ncc_xform_find_child_nt(pl,
                                                         "parameter_declaration");

    if (!first_pd) {
        return nullptr;
    }

    ncc_string_t text = ncc_xform_node_to_text(first_pd);

    if (!text.data) {
        return nullptr;
    }

    // Walk from the right past the name + whitespace + `*`.
    char *s   = text.data;
    int   n   = (int)text.u8_bytes;
    int   end = n;

    while (end > 0
           && (s[end - 1] == ' ' || s[end - 1] == '\t'
               || s[end - 1] == '\n')) {
        end--;
    }

    // Strip trailing identifier (param name).
    while (end > 0 && is_ident_char(s[end - 1])) {
        end--;
    }

    while (end > 0
           && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }

    // Strip the `*`.
    if (end == 0 || s[end - 1] != '*') {
        // Not the expected `T *name` shape.
        ncc_free(text.data);
        return nullptr;
    }

    end--;

    while (end > 0
           && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        end--;
    }

    char *out = ncc_alloc_size(1, (size_t)end + 1);
    memcpy(out, s, (size_t)end);
    out[end] = '\0';

    ncc_free(text.data);
    return out;
}

// ============================================================================
// Stream-shape detection
//
// Distinguishes the four stream shapes by leaf-text inspection of
// the return type and first parameter:
//
//   ret has `typeid("result", T *)`            +  param T *      → UNARY
//   ret has `typeid("result", n00b_rpc_stream_t(T) *)` + param T *
//                                              → SERVER_STREAM
//   ret has `typeid("result", T *)`            +  param n00b_rpc_stream_t(T) *
//                                              → CLIENT_STREAM
//   ret has `typeid("result", n00b_rpc_stream_t(T) *)`
//                                              +  param n00b_rpc_stream_t(T) *
//                                              → BIDI
// ============================================================================

typedef enum {
    RPC_SHAPE_UNKNOWN,
    RPC_SHAPE_UNARY,
    RPC_SHAPE_SERVER_STREAM,
    RPC_SHAPE_CLIENT_STREAM,
    RPC_SHAPE_BIDI,
} rpc_shape_t;

// Match a `typeid(<whitespace>?"<tag>"<whitespace>?,` substring in
// `text`. The leaf-text representation may include arbitrary
// whitespace between `typeid`, `(`, and the tag string.
static bool
text_has_typeid_tag(const char *text, const char *tag)
{
    if (!text) {
        return false;
    }

    size_t      tag_len = strlen(tag);
    const char *p       = text;

    while ((p = strstr(p, "typeid")) != nullptr) {
        const char *q = p + 6;

        while (*q == ' ' || *q == '\t' || *q == '\n') {
            q++;
        }

        if (*q != '(') {
            p++;
            continue;
        }

        q++;

        while (*q == ' ' || *q == '\t' || *q == '\n') {
            q++;
        }

        if (*q != '"') {
            p++;
            continue;
        }

        q++;

        if (strncmp(q, tag, tag_len) == 0 && q[tag_len] == '"') {
            return true;
        }

        p++;
    }

    return false;
}

// Peel a stream-of-T type → T. Recognizes both the libn00b macro
// form `n00b_rpc_stream_t(T)` (which downstream cpp will have
// expanded before ncc sees it, but show-up shape-wise in some
// edge cases) and the raw `_generic_struct typeid("rpc_stream", T)`
// form that the macro expands to. Returns nullptr if not a stream.
// Caller frees on success.
static char *
peel_stream_type(const char *text)
{
    if (!text) {
        return nullptr;
    }

    // Try the raw form via the typeid extractor.
    if (text_has_typeid_tag(text, "rpc_stream")) {
        return extract_typeid_inner(text, "rpc_stream");
    }

    // Fallback: literal `n00b_rpc_stream_t(T)` (would appear pre-cpp).
    static const char prefix[] = "n00b_rpc_stream_t";
    size_t            pre_len  = sizeof(prefix) - 1;
    const char       *p        = text;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (strncmp(p, prefix, pre_len) != 0) {
        return nullptr;
    }

    p += pre_len;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    if (*p != '(') {
        return nullptr;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    int         depth = 1;
    const char *start = p;

    while (*p && depth > 0) {
        if (*p == '(') {
            depth++;
        }
        else if (*p == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }

        p++;
    }

    if (depth != 0) {
        return nullptr;
    }

    const char *end = p;

    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    if (end == start) {
        return nullptr;
    }

    size_t len = (size_t)(end - start);
    char  *out = ncc_alloc_size(1, len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

// True iff the last parameter of `declarator`'s parameter list is
// of type `n00b_rpc_ctx_t *` (spec §5.2 requires every @rpc handler
// to take an `n00b_rpc_ctx_t *ctx` trailing parameter).
static bool
last_param_is_ctx(ncc_parse_tree_t *declarator)
{
    ncc_parse_tree_t *ptl = find_descendant_nt(declarator,
                                                "parameter_type_list");

    if (!ptl) {
        return false;
    }

    ncc_string_t text = ncc_xform_node_to_text(ptl);

    if (!text.data) {
        return false;
    }

    // Scan to the last top-level comma; the substring after it is
    // the last parameter's declaration.
    const char *s         = text.data;
    int         depth     = 0;
    size_t      last_comma = 0;
    bool        have_comma = false;

    for (size_t i = 0; i < text.u8_bytes; i++) {
        char c = s[i];

        if (c == '(' || c == '[' || c == '{') {
            depth++;
        }
        else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) {
                depth--;
            }
        }
        else if (c == ',' && depth == 0) {
            last_comma = i;
            have_comma = true;
        }
    }

    const char *last = have_comma ? s + last_comma + 1 : s;
    bool        ok   = strstr(last, "n00b_rpc_ctx_t") != nullptr;

    ncc_free(text.data);
    return ok;
}

// Decide which of the four shapes the handler matches. Also returns
// (via the out-pointers) the inner element types — caller frees both.
//
//   * `resp_inner_out`: T from `typeid("result", T *)` (unary,
//      client-stream) or T from `typeid("result", n00b_rpc_stream_t(T) *)`
//      (server-stream, bidi).
//   * `req_inner_out`: T from `T *req` (unary, server-stream) or T
//      from `n00b_rpc_stream_t(T) *in` (client-stream, bidi).
static rpc_shape_t
detect_shape(ncc_parse_tree_t *inner, char **resp_inner_out,
             char **req_inner_out)
{
    *resp_inner_out = nullptr;
    *req_inner_out  = nullptr;

    ncc_parse_tree_t *decl_specs
        = ncc_xform_find_child_nt(inner, "declaration_specifiers");
    ncc_parse_tree_t *declarator
        = ncc_xform_find_child_nt(inner, "declarator");

    if (!decl_specs || !declarator) {
        return RPC_SHAPE_UNKNOWN;
    }

    ncc_string_t ret_text = ncc_xform_node_to_text(decl_specs);

    if (!text_has_typeid_tag(ret_text.data, "result")) {
        ncc_free(ret_text.data);
        return RPC_SHAPE_UNKNOWN;
    }

    char *raw_resp = extract_typeid_inner(ret_text.data, "result");
    ncc_free(ret_text.data);

    if (!raw_resp) {
        return RPC_SHAPE_UNKNOWN;
    }

    bool resp_is_stream     = false;
    char *resp_inner        = peel_stream_type(raw_resp);

    if (resp_inner) {
        resp_is_stream = true;
        ncc_free(raw_resp);
    }
    else {
        resp_inner = raw_resp;
    }

    char *first_type = extract_first_param_type(declarator);

    if (!first_type) {
        ncc_free(resp_inner);
        return RPC_SHAPE_UNKNOWN;
    }

    bool req_is_stream = false;
    char *req_inner    = peel_stream_type(first_type);

    if (req_inner) {
        req_is_stream = true;
        ncc_free(first_type);
    }
    else {
        req_inner = first_type;
    }

    *resp_inner_out = resp_inner;
    *req_inner_out  = req_inner;

    if (!resp_is_stream && !req_is_stream) {
        return RPC_SHAPE_UNARY;
    }
    if (resp_is_stream && !req_is_stream) {
        return RPC_SHAPE_SERVER_STREAM;
    }
    if (!resp_is_stream && req_is_stream) {
        return RPC_SHAPE_CLIENT_STREAM;
    }
    return RPC_SHAPE_BIDI;
}

// ============================================================================
// Splice infrastructure (mirrors xform_once)
// ============================================================================

typedef struct {
    ncc_parse_tree_t **items;
    size_t             len;
    size_t             cap;
} ptrvec_t;

static void
ptrvec_init(ptrvec_t *v, size_t init_cap)
{
    v->cap   = init_cap > 0 ? init_cap : 16;
    v->len   = 0;
    v->items = ncc_alloc_array(ncc_parse_tree_t *, v->cap);
}

static void
ptrvec_push(ptrvec_t *v, ncc_parse_tree_t *p)
{
    if (v->len >= v->cap) {
        v->cap *= 2;
        v->items = ncc_realloc(v->items,
                                v->cap * sizeof(ncc_parse_tree_t *));
    }

    v->items[v->len++] = p;
}

static bool
is_group_node(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    ncc_nt_node_t pn = ncc_tree_node_value(node);
    return pn.group_top;
}

static void
flatten_group(ncc_parse_tree_t *node, ptrvec_t *out)
{
    if (!node) {
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        ptrvec_push(out, node);
        return;
    }

    if (is_group_node(node)) {
        size_t nc = ncc_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            flatten_group(ncc_tree_child(node, i), out);
        }
    }
    else {
        ptrvec_push(out, node);
    }
}

// ============================================================================
// Synthesize unary dispatcher / ctor / stub via template
// ============================================================================

// ============================================================================
// Per-shape source builders
//
// Each builder emits three external_declarations (dispatcher, ctor,
// client stub) wired through the appropriate runtime entry-points.
// The streaming variants exploit the runtime's structural alias
// between typed and buffer streams: dispatchers cast the inbound
// `n00b_rpc_stream_t(n00b_buffer_t *) *` to `n00b_rpc_stream_t(T) *`
// for the handler call, and cast the handler's returned typed
// stream back to buffer-stream form. The handler does per-item
// CBOR on send/recv.
// ============================================================================

static void
build_unary_source(ncc_buffer_t *src, const char *handler,
                   const char *dispatcher, const char *registrar,
                   const char *call_, const char *quoted_method,
                   const char *req_t, const char *resp_t)
{
    ncc_buffer_printf(src,
        "static _generic_struct typeid(\"result\", n00b_buffer_t *)\n"
        "%s(n00b_buffer_t *req_cbor, n00b_rpc_ctx_t *ctx)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", %s *) decoded =\n"
        "    typeid(\"cbor_decode\", %s *)(req_cbor);\n"
        "  if (!decoded.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", n00b_buffer_t *)){\n"
        "      .is_ok = false, .err = decoded.err,\n"
        "    };\n"
        "  }\n"
        "  _generic_struct typeid(\"result\", %s *) resp = %s(decoded.ok, ctx);\n"
        "  if (!resp.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", n00b_buffer_t *)){\n"
        "      .is_ok = false, .err = resp.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", n00b_buffer_t *)){\n"
        "    .is_ok = true,\n"
        "    .ok = typeid(\"cbor_encode\", %s *)(resp.ok),\n"
        "  };\n"
        "}\n"
        "__attribute__((constructor))\n"
        "static void %s(void) { n00b_rpc_register(%s, &%s); }\n"
        "extern _generic_struct typeid(\"result\", %s *)\n"
        "%s(n00b_rpc_ctx_t *ctx, n00b_rpc_channel_t *chan, %s *req)\n"
        "{\n"
        "  n00b_buffer_t *req_buf = typeid(\"cbor_encode\", %s *)(req);\n"
        "  _generic_struct typeid(\"result\", n00b_buffer_t *) wire =\n"
        "    n00b_rpc_call_unary(ctx, chan, %s, req_buf);\n"
        "  if (!wire.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", %s *)){\n"
        "      .is_ok = false, .err = wire.err,\n"
        "    };\n"
        "  }\n"
        "  return typeid(\"cbor_decode\", %s *)(wire.ok);\n"
        "}\n",
        dispatcher,
        req_t, req_t,
        resp_t, handler,
        resp_t,
        registrar, quoted_method, dispatcher,
        resp_t, call_, req_t, req_t,
        quoted_method,
        resp_t,
        resp_t);
}

// Shorthand used in the streaming builders below. `n00b_rpc_stream_t(T)`
// is a libn00b macro that expands to `_generic_struct typeid("rpc_stream", T)`;
// since template-synthesized text is parsed without cpp, we spell the
// raw form everywhere.
#define STREAM_OF_BUF \
    "_generic_struct typeid(\"rpc_stream\", n00b_buffer_t *)"
#define STREAM_OF "_generic_struct typeid(\"rpc_stream\", %s)"

static void
build_server_stream_source(ncc_buffer_t *src, const char *handler,
                            const char *dispatcher, const char *registrar,
                            const char *call_, const char *quoted_method,
                            const char *req_t, const char *resp_t)
{
    ncc_buffer_printf(src,
        "static _generic_struct typeid(\"result\", " STREAM_OF_BUF " *)\n"
        "%s(n00b_buffer_t *req_cbor, n00b_rpc_ctx_t *ctx)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", %s *) decoded =\n"
        "    typeid(\"cbor_decode\", %s *)(req_cbor);\n"
        "  if (!decoded.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", " STREAM_OF_BUF " *)){\n"
        "      .is_ok = false, .err = decoded.err,\n"
        "    };\n"
        "  }\n"
        "  _generic_struct typeid(\"result\", " STREAM_OF " *) resp =\n"
        "    %s(decoded.ok, ctx);\n"
        "  if (!resp.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", " STREAM_OF_BUF " *)){\n"
        "      .is_ok = false, .err = resp.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", " STREAM_OF_BUF " *)){\n"
        "    .is_ok = true,\n"
        "    .ok    = (" STREAM_OF_BUF " *)resp.ok,\n"
        "  };\n"
        "}\n"
        "__attribute__((constructor))\n"
        "static void %s(void) { n00b_rpc_register_server_stream(%s, &%s); }\n"
        "extern _generic_struct typeid(\"result\", " STREAM_OF " *)\n"
        "%s(n00b_rpc_ctx_t *ctx, n00b_rpc_channel_t *chan, %s *req)\n"
        "{\n"
        "  n00b_buffer_t *req_buf = typeid(\"cbor_encode\", %s *)(req);\n"
        "  _generic_struct typeid(\"result\", " STREAM_OF_BUF " *) wire =\n"
        "    n00b_rpc_call_server_stream(ctx, chan, %s, req_buf);\n"
        "  if (!wire.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", " STREAM_OF " *)){\n"
        "      .is_ok = false, .err = wire.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", " STREAM_OF " *)){\n"
        "    .is_ok = true,\n"
        "    .ok    = (" STREAM_OF " *)wire.ok,\n"
        "  };\n"
        "}\n",
        dispatcher,
        req_t, req_t,
        resp_t, handler,
        registrar, quoted_method, dispatcher,
        resp_t, call_, req_t, req_t,
        quoted_method,
        resp_t,
        resp_t, resp_t);
}

static void
build_client_stream_source(ncc_buffer_t *src, const char *handler,
                            const char *dispatcher, const char *registrar,
                            const char *call_, const char *quoted_method,
                            const char *req_t, const char *resp_t)
{
    ncc_buffer_printf(src,
        "static _generic_struct typeid(\"result\", n00b_buffer_t *)\n"
        "%s(" STREAM_OF_BUF " *in, n00b_rpc_ctx_t *ctx)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", %s *) resp =\n"
        "    %s((" STREAM_OF " *)in, ctx);\n"
        "  if (!resp.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", n00b_buffer_t *)){\n"
        "      .is_ok = false, .err = resp.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", n00b_buffer_t *)){\n"
        "    .is_ok = true,\n"
        "    .ok = typeid(\"cbor_encode\", %s *)(resp.ok),\n"
        "  };\n"
        "}\n"
        "__attribute__((constructor))\n"
        "static void %s(void) { n00b_rpc_register_client_stream(%s, &%s); }\n"
        "extern _generic_struct typeid(\"result\", %s *)\n"
        "%s(n00b_rpc_ctx_t *ctx, n00b_rpc_channel_t *chan, " STREAM_OF " *in)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", n00b_buffer_t *) wire =\n"
        "    n00b_rpc_call_client_stream(ctx, chan, %s, (" STREAM_OF_BUF " *)in);\n"
        "  if (!wire.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", %s *)){\n"
        "      .is_ok = false, .err = wire.err,\n"
        "    };\n"
        "  }\n"
        "  return typeid(\"cbor_decode\", %s *)(wire.ok);\n"
        "}\n",
        dispatcher,
        resp_t, handler, req_t,
        resp_t,
        registrar, quoted_method, dispatcher,
        resp_t, call_, req_t,
        quoted_method,
        resp_t,
        resp_t);
}

static void
build_bidi_source(ncc_buffer_t *src, const char *handler,
                  const char *dispatcher, const char *registrar,
                  const char *call_, const char *quoted_method,
                  const char *req_t, const char *resp_t)
{
    ncc_buffer_printf(src,
        "static _generic_struct typeid(\"result\", " STREAM_OF_BUF " *)\n"
        "%s(" STREAM_OF_BUF " *in, n00b_rpc_ctx_t *ctx)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", " STREAM_OF " *) resp =\n"
        "    %s((" STREAM_OF " *)in, ctx);\n"
        "  if (!resp.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", " STREAM_OF_BUF " *)){\n"
        "      .is_ok = false, .err = resp.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", " STREAM_OF_BUF " *)){\n"
        "    .is_ok = true,\n"
        "    .ok    = (" STREAM_OF_BUF " *)resp.ok,\n"
        "  };\n"
        "}\n"
        "__attribute__((constructor))\n"
        "static void %s(void) { n00b_rpc_register_bidi(%s, &%s); }\n"
        "extern _generic_struct typeid(\"result\", " STREAM_OF " *)\n"
        "%s(n00b_rpc_ctx_t *ctx, n00b_rpc_channel_t *chan, " STREAM_OF " *in)\n"
        "{\n"
        "  _generic_struct typeid(\"result\", " STREAM_OF_BUF " *) wire =\n"
        "    n00b_rpc_call_bidi(ctx, chan, %s, (" STREAM_OF_BUF " *)in);\n"
        "  if (!wire.is_ok) {\n"
        "    return (_generic_struct typeid(\"result\", " STREAM_OF " *)){\n"
        "      .is_ok = false, .err = wire.err,\n"
        "    };\n"
        "  }\n"
        "  return (_generic_struct typeid(\"result\", " STREAM_OF " *)){\n"
        "    .is_ok = true,\n"
        "    .ok    = (" STREAM_OF " *)wire.ok,\n"
        "  };\n"
        "}\n",
        dispatcher,
        resp_t, handler, req_t,
        registrar, quoted_method, dispatcher,
        resp_t, call_, req_t,
        quoted_method,
        resp_t,
        resp_t, resp_t);
}

#undef STREAM_OF_BUF
#undef STREAM_OF

// ============================================================================
// Unified synthesizer — picks the right per-shape builder, runs the
// shared metadata extraction + parse + splice.
// ============================================================================

static void
synthesize_for_shape(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *inner,
                     ncc_parse_tree_t *rpc_clause,
                     const char *method_string,
                     rpc_shape_t shape,
                     char *req_inner, char *resp_inner,
                     ptrvec_t *appended)
{
    ncc_parse_tree_t *declarator
        = ncc_xform_find_child_nt(inner, "declarator");

    char *func_name = extract_func_name(declarator);

    if (!func_name) {
        rpc_diagnostic(rpc_clause,
                       "@rpc could not determine the handler's "
                       "function name");
    }

    char *svc;
    char *method;
    split_method(method_string, &svc, &method);

    size_t  ms_len    = strlen(method_string);
    char   *quoted_ms = ncc_alloc_size(1, ms_len + 3);
    quoted_ms[0]      = '"';
    memcpy(quoted_ms + 1, method_string, ms_len);
    quoted_ms[ms_len + 1] = '"';
    quoted_ms[ms_len + 2] = '\0';

    size_t svc_len    = strlen(svc);
    size_t method_len = strlen(method);

    #define BUILD_NAME(prefix, suffix_out) do {                  \
        size_t p_len = strlen(prefix);                          \
        size_t total = p_len + svc_len + 2 + method_len + 1;    \
        (suffix_out) = ncc_alloc_size(1, total);                \
        memcpy((suffix_out), (prefix), p_len);                   \
        memcpy((suffix_out) + p_len, svc, svc_len);              \
        (suffix_out)[p_len + svc_len]     = '_';                 \
        (suffix_out)[p_len + svc_len + 1] = '_';                 \
        memcpy((suffix_out) + p_len + svc_len + 2,               \
               method, method_len);                              \
        (suffix_out)[total - 1] = '\0';                          \
    } while (0)

    char *dispatcher_name;
    char *registrar_name;
    char *call_name;
    BUILD_NAME("_n00b_rpc_dispatch__", dispatcher_name);
    BUILD_NAME("_n00b_rpc_register__", registrar_name);
    BUILD_NAME("n00b_rpc_call_",       call_name);
    #undef BUILD_NAME

    ncc_buffer_t *src = ncc_buffer_empty();

    switch (shape) {
    case RPC_SHAPE_UNARY:
        build_unary_source(src, func_name, dispatcher_name,
                            registrar_name, call_name, quoted_ms,
                            req_inner, resp_inner);
        break;
    case RPC_SHAPE_SERVER_STREAM:
        build_server_stream_source(src, func_name, dispatcher_name,
                                    registrar_name, call_name,
                                    quoted_ms, req_inner, resp_inner);
        break;
    case RPC_SHAPE_CLIENT_STREAM:
        build_client_stream_source(src, func_name, dispatcher_name,
                                    registrar_name, call_name,
                                    quoted_ms, req_inner, resp_inner);
        break;
    case RPC_SHAPE_BIDI:
        build_bidi_source(src, func_name, dispatcher_name,
                           registrar_name, call_name, quoted_ms,
                           req_inner, resp_inner);
        break;
    default:
        rpc_diagnostic(rpc_clause,
                       "@rpc: handler has an unrecognized signature "
                       "shape");
    }

    char *source = ncc_buffer_take(src);

    ncc_parse_tree_t *new_tu
        = ncc_xform_parse_source(ctx->grammar, "translation_unit",
                                 source, "xform_rpc");
    ncc_free(source);

    if (!new_tu) {
        rpc_diagnostic(rpc_clause,
                       "internal error: failed to parse synthesized "
                       "@rpc source");
    }

    ptrvec_t flat;
    ptrvec_init(&flat, 8);

    size_t nc = ncc_tree_num_children(new_tu);

    for (size_t i = 0; i < nc; i++) {
        flatten_group(ncc_tree_child(new_tu, i), &flat);
    }

    for (size_t i = 0; i < flat.len; i++) {
        if (flat.items[i]) {
            ptrvec_push(appended, flat.items[i]);
        }
    }

    ncc_free(flat.items);
    ncc_free(func_name);
    ncc_free(svc);
    ncc_free(method);
    ncc_free(quoted_ms);
    ncc_free(dispatcher_name);
    ncc_free(registrar_name);
    ncc_free(call_name);
}

// ============================================================================
// Pre-order transform on translation_unit
//
// Strategy:
//   1. Flatten the TU's `$$group_*` wrapper into a flat list of
//      external_declarations.
//   2. For each: strip @rpc (validates, diagnoses, removes the
//      clause from the parent), synthesize the matching dispatcher /
//      registrar / client stub, and queue its declarations for append.
//   3. Rebuild the TU as `[originals... ; appended...]`, wrapped in
//      a single fresh `$$group_rpc` node — the standard ncc idiom
//      for replacing TU children in bulk.
// ============================================================================

static ncc_parse_tree_t *
xform_rpc_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
             [[maybe_unused]] ncc_xform_control_t *control)
{
    ptrvec_t flat;
    ptrvec_init(&flat, 256);

    size_t tu_nc = ncc_tree_num_children(tu);

    for (size_t i = 0; i < tu_nc; i++) {
        flatten_group(ncc_tree_child(tu, i), &flat);
    }

    ptrvec_t appended;
    ptrvec_init(&appended, 16);

    // Per-TU registry of @rpc method strings already seen; used to
    // diagnose duplicate `@rpc("same/method")` annotations. Each
    // entry is a heap-copy of the method string we own.
    ptrvec_t seen_methods;
    ptrvec_init(&seen_methods, 8);

    bool changed = false;

    for (size_t i = 0; i < flat.len; i++) {
        ncc_parse_tree_t *ext_decl = flat.items[i];

        if (!ext_decl || ncc_tree_is_leaf(ext_decl)) {
            continue;
        }

        // Find the inner function_definition / declaration.
        ncc_parse_tree_t *inner    = nullptr;
        size_t            inner_nc = ncc_tree_num_children(ext_decl);

        for (size_t j = 0; j < inner_nc; j++) {
            ncc_parse_tree_t *c = ncc_tree_child(ext_decl, j);

            if (c && !ncc_tree_is_leaf(c)) {
                inner = c;
                break;
            }
        }

        if (!inner
            || (!ncc_xform_nt_name_is(inner, "function_definition")
                && !ncc_xform_nt_name_is(inner, "declaration"))) {
            continue;
        }
        inner = unwrap_extension_function_definition(inner);

        // Locate the @rpc clause without removing it yet — we need
        // the clause node alive for shape-detection diagnostics.
        size_t            rpc_idx;
        ncc_parse_tree_t *rpc_clause
            = find_child_nt_index(inner, "rpc_clause", &rpc_idx);

        if (!rpc_clause) {
            continue;
        }

        if (find_child_nt_index(inner, "function_contract_clauses",
                                nullptr)) {
            rpc_diagnostic(rpc_clause,
                           "@rpc cannot be combined with function "
                           "contracts on the same function");
        }

        // _kargs combination diagnostic (spec §4).
        if (find_child_nt_index(inner, "keyword_clause", nullptr)) {
            rpc_diagnostic(rpc_clause,
                           "@rpc cannot be combined with _kargs on the "
                           "same function");
        }

        char *method_string = extract_method_string(rpc_clause);

        if (!method_string) {
            rpc_diagnostic(rpc_clause,
                           "@rpc requires a string literal of the form "
                           "\"package.Service/Method\"");
        }

        if (!validate_method_string(method_string)) {
            rpc_diagnostic(rpc_clause,
                           "@rpc method string \"%s\" does not match "
                           "\"<package>.<Service>/<Method>\"",
                           method_string);
        }

        // Only synthesize for function_definition (the prototype
        // declaration also gets stripped, but we don't double-emit
        // the dispatcher / ctor / stub).
        if (ncc_xform_nt_name_is(inner, "function_definition")) {
            // Check for a duplicate `@rpc(method)` earlier in the TU.
            for (size_t k = 0; k < seen_methods.len; k++) {
                if (strcmp((const char *)seen_methods.items[k],
                            method_string)
                    == 0) {
                    rpc_diagnostic(rpc_clause,
                                   "@rpc method \"%s\" is already bound "
                                   "by another handler in this "
                                   "translation unit",
                                   method_string);
                }
            }

            // Take ownership of method_string in seen_methods (we
            // free the dup list at the end of the TU walk). Use a
            // copy so the original can still be freed after the
            // call to synthesize_for_shape.
            size_t ms_len = strlen(method_string);
            char  *kept   = ncc_alloc_size(1, ms_len + 1);
            memcpy(kept, method_string, ms_len + 1);
            ptrvec_push(&seen_methods, (ncc_parse_tree_t *)kept);

            ncc_parse_tree_t *declarator
                = ncc_xform_find_child_nt(inner, "declarator");

            if (!declarator || !last_param_is_ctx(declarator)) {
                rpc_diagnostic(rpc_clause,
                               "@rpc handler must take a trailing "
                               "n00b_rpc_ctx_t * parameter (saw none)");
            }

            char       *req_inner  = nullptr;
            char       *resp_inner = nullptr;
            rpc_shape_t shape      = detect_shape(inner, &resp_inner,
                                                  &req_inner);

            if (shape == RPC_SHAPE_UNKNOWN) {
                rpc_diagnostic(rpc_clause,
                               "@rpc: handler signature does not match "
                               "any supported shape (expected return "
                               "n00b_result_t(T *) or "
                               "n00b_result_t(n00b_rpc_stream_t(T) *), "
                               "with first parameter T * or "
                               "n00b_rpc_stream_t(T) *)");
            }

            synthesize_for_shape(ctx, inner, rpc_clause, method_string,
                                 shape, req_inner, resp_inner,
                                 &appended);

            ncc_free(req_inner);
            ncc_free(resp_inner);
        }

        ncc_free(method_string);
        ncc_xform_remove_child(inner, rpc_idx);
        changed = true;
    }

    for (size_t i = 0; i < seen_methods.len; i++) {
        ncc_free(seen_methods.items[i]);
    }
    ncc_free(seen_methods.items);

    if (!changed) {
        ncc_free(flat.items);
        ncc_free(appended.items);
        return nullptr;
    }

    // Rebuild the TU: originals first, synthesized second, all
    // under one fresh `$$group_rpc` wrapper.
    ncc_nt_node_t gpn = {0};
    gpn.name          = ncc_string_from_cstr("$$group_rpc");
    gpn.id            = (1 << 28);
    gpn.group_top     = true;

    ncc_parse_tree_t *new_group
        = ncc_tree_node(ncc_nt_node_t, ncc_token_info_ptr_t, gpn);

    for (size_t i = 0; i < flat.len; i++) {
        if (flat.items[i]) {
            ncc_tree_add_child(new_group, flat.items[i]);
        }
    }

    for (size_t i = 0; i < appended.len; i++) {
        if (appended.items[i]) {
            ncc_tree_add_child(new_group, appended.items[i]);
        }
    }

    ncc_free(flat.items);
    ncc_free(appended.items);

    ncc_tree_replace_children(tu, ncc_alloc_array(ncc_parse_tree_t *, 1),
                              1);
    ncc_tree_set_child(tu, 0, new_group);

    ctx->nodes_replaced++;
    return tu;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_register_rpc_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "translation_unit", xform_rpc_tu, "rpc");
}
