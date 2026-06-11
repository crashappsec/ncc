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

// Append the type-relevant specifiers of a declaration_specifiers subtree to
// `buf`, in order, dropping storage-class and function specifiers (which are
// part of the declaration but not the return type). `*first` tracks spacing.
//
//   declaration_specifier ::= storage_class_specifier
//       | type_specifier_qualifier | function_specifier
//
// The fix that lets a bare typedef-name return type after `static` parse as a
// type_specifier_qualifier (rather than mis-classifying as a function
// specifier) is what makes this classification trustworthy.
static void
collect_type_specs(ncc_parse_tree_t *n, ncc_buffer_t *buf, bool *first)
{
    if (!n || ncc_tree_is_leaf(n)) {
        return;
    }
    if (ncc_xform_nt_name_is(n, "declaration_specifier")) {
        ncc_parse_tree_t *inner = ncc_tree_child(n, 0);
        if (inner && ncc_xform_nt_name_is(inner, "type_specifier_qualifier")) {
            ncc_string_t t = ncc_xform_node_to_text(inner);
            if (t.data) {
                if (!*first) {
                    ncc_buffer_putc(buf, ' ');
                }
                ncc_buffer_puts(buf, t.data);
                *first = false;
                ncc_free(t.data);
            }
        }
        return; // classified — do not descend into the specifier's own body
    }
    size_t nc = ncc_tree_num_children(n);
    for (size_t i = 0; i < nc; i++) {
        collect_type_specs(ncc_tree_child(n, i), buf, first);
    }
}

// The enclosing function's return type spelling: its declaration_specifiers
// with storage-class / function specifiers dropped, plus any leading pointer on
// the declarator. Caller frees. Returns nullptr on failure.
static char *
return_type_text(ncc_parse_tree_t *func)
{
    ncc_parse_tree_t *specs = ncc_xform_find_child_nt(func,
                                                      "declaration_specifiers");
    if (!specs) {
        return nullptr;
    }

    ncc_buffer_t *buf   = ncc_buffer_empty();
    bool          first = true;
    collect_type_specs(specs, buf, &first);
    if (first) {
        ncc_free(ncc_buffer_take(buf)); // no type specifier found
        return nullptr;
    }

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
