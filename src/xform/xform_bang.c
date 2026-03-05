// xform_bang.c — Transform: postfix `!` (bang) operator.
//
// Provides Rust-style error propagation for Result types.
//
//   result_t foo(void) {
//       int val = bar()!;
//   }
//
// becomes:
//
//   result_t foo(void) {
//       int val = ({
//           __auto_type _ncc_try_1 = (bar());
//           if (!_ncc_try_1.is_ok) {
//               return (result_t){ .is_ok = false, .err = _ncc_try_1.err };
//           }
//           _ncc_try_1.ok;
//       });
//   }
//
// Registered as post-order on "postfix_expression".

#include "xform/xform_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

// Collect return type text from declaration_specifiers, skipping
// only storage_class_specifier subtrees. We include function_specifier
// leaves because the parser may classify typedef names there.
static void
collect_type_text(ncc_parse_tree_t *node, char *buf, size_t cap, size_t *pos)
{
    if (!node) {
        return;
    }
    if (ncc_tree_is_leaf(node)) {
        const char *text = ncc_xform_leaf_text(node);
        if (text) {
            // Skip known function specifiers that aren't type names.
            if (strcmp(text, "inline") == 0
                || strcmp(text, "_Noreturn") == 0
                || strcmp(text, "noreturn") == 0
                || strcmp(text, "_Once") == 0
                || strcmp(text, "__inline__") == 0
                || strcmp(text, "__inline") == 0) {
                return;
            }
            size_t tlen = strlen(text);
            if (*pos + tlen + 2 < cap) {
                if (*pos > 0) {
                    buf[(*pos)++] = ' ';
                }
                memcpy(buf + *pos, text, tlen);
                *pos += tlen;
                buf[*pos] = '\0';
            }
        }
        return;
    }
    if (ncc_xform_nt_name_is(node, "storage_class_specifier")) {
        return;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_type_text(ncc_tree_child(node, i), buf, cap, pos);
    }
}

// Check if declaration_specifiers has void type (excluding storage/function).
static bool
has_void_type(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, "void");
    }
    if (ncc_xform_nt_name_is(node, "storage_class_specifier")
        || ncc_xform_nt_name_is(node, "function_specifier")) {
        return false;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (has_void_type(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

// Template parsing helper.
static ncc_parse_tree_t *
parse_template(ncc_grammar_t *g, const char *nt_name, const char *src)
{
    ncc_result_t(ncc_parse_tree_ptr_t) r =
        ncc_xform_parse_template(g, nt_name, src, nullptr);
    if (ncc_result_is_err(r)) {
        fprintf(stderr,
                "xform_bang: template parse failed for '%s':\n  %s\n",
                nt_name, src);
        return nullptr;
    }
    return ncc_result_get(r);
}

// ============================================================================
// Post-order transform on postfix_expression
// ============================================================================

static ncc_parse_tree_t *
xform_bang(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    size_t nc = ncc_tree_num_children(node);
    if (nc < 2) {
        return nullptr;
    }

    ncc_parse_tree_t *last_child = ncc_tree_child(node, nc - 1);
    if (!last_child || !ncc_tree_is_leaf(last_child)) {
        return nullptr;
    }
    if (!ncc_xform_leaf_text_eq(last_child, "!")) {
        return nullptr;
    }

    // Found a postfix ! expression.

    // Find the enclosing function via parent pointers.
    ncc_parse_tree_t *func_def = ncc_xform_find_ancestor(
        node, "function_definition");

    if (!func_def) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: postfix '!' used outside of a function "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
    }

    // Extract the return type from the function's declaration_specifiers.
    ncc_parse_tree_t *decl_specs = ncc_xform_find_child_nt(
        func_def, "declaration_specifiers");

    if (!decl_specs) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: postfix '!' in function with no return type "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
    }

    if (has_void_type(decl_specs)) {
        uint32_t line, col;
        ncc_xform_first_leaf_pos(node, &line, &col);
        fprintf(stderr,
                "ncc: error: postfix '!' in void function "
                "(line %u, col %u)\n",
                line, col);
        exit(1);
    }

    char ret_type[256] = {0};
    size_t rpos = 0;
    collect_type_text(decl_specs, ret_type, sizeof(ret_type), &rpos);
    if (rpos == 0) {
        strcpy(ret_type, "int");
    }

    // Get the operand text (everything before the !).
    // The operand is child[0] through child[nc-2]. For most cases it's
    // just child[0], but for chained postfix like foo()! we still want
    // the entire prefix. Collect text from all children except the last.
    char operand_buf[4096] = {0};
    size_t opos = 0;

    for (size_t i = 0; i < nc - 1; i++) {
        ncc_string_t part = ncc_xform_node_to_text(ncc_tree_child(node, i));
        if (part.data) {
            size_t plen = part.u8_bytes;
            if (opos + plen + 2 < sizeof(operand_buf)) {
                if (opos > 0) {
                    operand_buf[opos++] = ' ';
                }
                memcpy(operand_buf + opos, part.data, plen);
                opos += plen;
                operand_buf[opos] = '\0';
            }
            ncc_free(part.data);
        }
    }

    // Generate unique variable name.
    int id = ctx->unique_id++;
    char var_name[64];
    snprintf(var_name, sizeof(var_name), "_ncc_try_%d", id);

    // Build the statement expression template.
    char src[8192];
    snprintf(src, sizeof(src),
        "({ __auto_type %s = (%s);"
        " if (!%s.is_ok) {"
        " return (%s){ .is_ok = 0, .err = %s.err };"
        " }"
        " %s.ok; })",
        var_name, operand_buf,
        var_name,
        ret_type, var_name,
        var_name);

    // Parse as primary_expression (statement expression is a primary expr).
    ncc_parse_tree_t *replacement = parse_template(
        ctx->grammar, "primary_expression", src);

    if (!replacement) {
        fprintf(stderr,
                "ncc: error: failed to parse bang expansion template\n");
        exit(1);
    }

    return replacement;
}

// ============================================================================
// Registration
// ============================================================================

void
ncc_register_bang_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "postfix_expression", xform_bang, "bang");
}
