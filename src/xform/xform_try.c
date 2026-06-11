// xform_try.c — `_try <expr>` error propagation for n00b_result_t.
//
// `_try E` evaluates E (a n00b_result_t(T)); if the result is an error, the
// enclosing function returns that error (propagation); otherwise `_try E`
// yields the unwrapped `.ok` value. Spelled `_try` rather than `try` because
// n00b uses `try` as an ordinary identifier.
//
// `_try` is an expression, so it lowers to a statement expression that can
// early-return from the enclosing function (a `return` inside `({ ... })`
// returns from the function under clang):
//
//     _try E   ->   ({
//         typeof (E) __ncc_try_N = (E);
//         if (!__ncc_try_N.is_ok) {
//             return (<enclosing-return-type>){ .is_ok = false,
//                                               .err = __ncc_try_N.err };
//         }
//         __ncc_try_N.ok;
//     })
//
// E is evaluated exactly once. The result shape (.is_ok / .ok / .err) matches
// n00b_result_t and the propagation already emitted by xform_rpc. The pass runs
// early so the emitted `return`, the enclosing `_generic_struct` result type,
// and any type-queries inside E still flow through the later passes (defer,
// generic_struct, typeid, gc, ...).

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/parse_tree.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ncc_try_counter = 0;

// First non-leaf child — the operand expression of `_try <operand>`.
static ncc_parse_tree_t *
try_operand(ncc_parse_tree_t *node)
{
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (c && !ncc_tree_is_leaf(c)) {
            return c;
        }
    }
    return nullptr;
}

// Walk up to the enclosing function_definition.
static ncc_parse_tree_t *
enclosing_function(ncc_parse_tree_t *node)
{
    for (ncc_parse_tree_t *p = node; p;) {
        ncc_nt_node_t v = ncc_tree_node_value(p);
        p               = v.parent;
        if (p && !ncc_tree_is_leaf(p)
            && ncc_xform_nt_name_is(p, "function_definition")) {
            return p;
        }
    }
    return nullptr;
}

// A leading word in declaration_specifiers that is a storage-class or
// function specifier, not part of the type. (A bare typedef-name return type
// after `static` can mis-parse as a function_specifier, so we strip by text
// rather than trusting the specifier classification.)
static bool
is_specifier_keyword(const char *w, size_t len)
{
    static const char *kw[] = {
        "static",   "extern",        "auto",      "register",
        "typedef",  "_Thread_local", "thread_local", "constexpr",
        "inline",   "_Noreturn",     "__inline",  "__inline__",
        "__forceinline",
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strlen(kw[i]) == len && memcmp(w, kw[i], len) == 0) {
            return true;
        }
    }
    return false;
}

// The enclosing function's return type spelling: the declaration_specifiers
// text with leading storage-class / function specifiers stripped, plus any
// leading pointer on the declarator. Caller frees. Returns nullptr on failure.
static char *
return_type_text(ncc_parse_tree_t *func)
{
    ncc_parse_tree_t *specs = ncc_xform_find_child_nt(func,
                                                      "declaration_specifiers");
    if (!specs) {
        return nullptr;
    }
    ncc_string_t spec_text = ncc_xform_node_to_text(specs);
    if (!spec_text.data) {
        return nullptr;
    }

    // Skip leading specifier-keyword words; everything from the first
    // non-keyword word on is the type.
    const char *p = spec_text.data;
    for (;;) {
        while (*p == ' ') {
            p++;
        }
        const char *w = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t len = (size_t)(p - w);
        if (len == 0) {
            break; // only specifiers — no type (shouldn't happen)
        }
        if (!is_specifier_keyword(w, len)) {
            p = w;
            break;
        }
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_puts(buf, p);
    ncc_free(spec_text.data);

    ncc_parse_tree_t *declr = ncc_xform_find_child_nt(func, "declarator");
    ncc_parse_tree_t *ptr   = declr ? ncc_xform_find_child_nt(declr, "pointer")
                                    : nullptr;
    if (ptr) {
        ncc_string_t pt = ncc_xform_node_to_text(ptr);
        if (pt.data) {
            ncc_buffer_putc(buf, ' ');
            ncc_buffer_puts(buf, pt.data);
            ncc_free(pt.data);
        }
    }
    return ncc_buffer_take(buf);
}

static ncc_parse_tree_t *
xform_try(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    ncc_parse_tree_t *operand = try_operand(node);
    if (!operand) {
        return nullptr;
    }

    ncc_parse_tree_t *func = enclosing_function(node);
    if (!func) {
        uint32_t line = 0;
        uint32_t col  = 0;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: _try is only valid inside a function "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
    }

    char *ret = return_type_text(func);
    if (!ret) {
        uint32_t line = 0;
        uint32_t col  = 0;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: _try: cannot determine the enclosing function's "
                "result return type (line %u, col %u)\n",
                line, col);
        exit(1);
    }

    ncc_string_t etext = ncc_xform_node_to_text(operand);
    const char  *e     = etext.data ? etext.data : "";
    int          n     = ncc_try_counter++;

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf,
                      "({ typeof (%s) __ncc_try_%d = (%s); "
                      "if (! __ncc_try_%d.is_ok) { return (%s){ .is_ok = false, "
                      ".err = __ncc_try_%d.err }; } "
                      "__ncc_try_%d.ok; })",
                      e, n, e, n, ret, n, n);

    if (etext.data) {
        ncc_free(etext.data);
    }
    ncc_free(ret);

    char *src = ncc_buffer_take(buf);
    ncc_parse_tree_t *result = ncc_xform_parse_source(
        ctx->grammar, "unary_expression", src, "xform_try");
    ncc_free(src);

    return result;
}

void
ncc_register_try_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "try_expression", xform_try, "try");
}
