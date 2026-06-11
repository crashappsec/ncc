// xform_nullable.c — the `?` nullable type qualifier.
//
// ncc accepts `?` as a type qualifier (like const), marking a pointer as
// "may be null": `?T x`, `T *? p`, `?T f(?T p)`. clang does not understand `?`,
// so this pass strips every `?` qualifier from the parse tree before emit. The
// nullability information itself is consumed by the (forthcoming) flow-sensitive
// null-state analysis, which reads it from the symbol table built before this
// strip runs; here we only make the annotated source compile.
//
// Stage 1 of the nullability feature: syntax + strip. The CFG, null-state
// dataflow, deref diagnostics, and `?T` parameter/return propagation build on
// top of this marker.

#include "lib/alloc.h"
#include "parse/parse_tree.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <string.h>

// True if `node` is the lone `?` nullable qualifier (a non-leaf wrapper whose
// entire text is "?"). The ternary `?` is a bare leaf directly under a
// conditional_expression, never a non-leaf whose text is exactly "?", so it is
// never matched here.
static bool
is_nullable_qualifier(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    ncc_string_t t = ncc_xform_node_to_text(node);
    if (!t.data) {
        return false;
    }
    const char *s = t.data;
    while (*s == ' ') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        len--;
    }
    bool match = (len == 1 && s[0] == '?');
    ncc_free(t.data);
    return match;
}

// Recursively remove `?` qualifier subtrees. Removing the smallest subtree whose
// text is exactly "?" drops the qualifier whether it sits in a
// declaration_specifiers list, a struct member's specifier list, or a pointer's
// type_qualifier_list.
static void
strip_nullable(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return;
    }

    size_t i = 0;
    while (i < ncc_tree_num_children(node)) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);
        if (is_nullable_qualifier(child)) {
            ncc_xform_remove_child(node, i);
            continue; // do not advance; children shifted down
        }
        strip_nullable(child);
        i++;
    }
}

static ncc_parse_tree_t *
xform_nullable_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *node)
{
    (void)ctx;
    strip_nullable(node);
    return node;
}

void
ncc_register_nullable_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register(reg, "translation_unit", xform_nullable_tu, "nullable");
}
