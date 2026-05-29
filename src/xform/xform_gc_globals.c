// xform_gc_globals.c — Transform: auto-register TU-scope pointer-
// bearing globals as libn00b GC roots.
//
// See include/xform/xform_gc_globals.h for the feature overview.
//
// Walk-order placement: registered LAST in the xform sequence (after
// rpc / generic_struct / typeid / option / typestr / typehash /
// kargs_vargs / once / bang / rstr / static_image / gc_stack_maps /
// constexpr / constexpr_paste — see src/ncc.c). The auto-roots pass
// must see the final flattened translation_unit, including any
// TU-scope synthetic decls earlier passes have produced
// (spec § 8.3). The placement is enforced at the registration site
// (ncc.c) rather than here; this comment documents the contract for
// future readers.
//
// Phase 2 scope was pointer scalars only. Phases 3+4+5 combined (per
// D-017) extend to:
//   * Pointer arrays (`T *x[N];`)  — one entry, `num_words = N`.
//   * Pointer-bearing structs       — one entry per pointer-run
//     (adjacent pointer-bearing words coalesce; non-pointer-bearing
//     words break the run). The address-arithmetic emit uses
//     `(void *)&(name).field.path`. We considered the canonical
//     `(void *)((char *)&name + __builtin_offsetof(typeof(name),
//     field.path))` form but ncc's grammar restricts the
//     `__builtin_offsetof` second argument to a single
//     <identifier> (c_ncc.bnf line 599) — it cannot round-trip a
//     nested designator through the post-transform reparse. Clang's
//     address-of-a-designated-subobject yields the same byte
//     address, so the simpler form is semantically equivalent and
//     parses cleanly.
//   * `[[n00b::nomap]]` attribute   — TU-scope decl carrying the
//     attribute (prefix or trailing) is silently skipped; the
//     attribute is stripped from emitted C.
//   * Incomplete-struct warn-and-skip (D-009) — TU-scope decl whose
//     struct type cannot be resolved emits a `gc_globals_warnf`
//     warning (severity `warning` per D-013) and is skipped.
//
// When `--ncc-auto-gc-roots` is absent, the pass returns immediately
// — the cheapest possible no-op (no allocation, no walk, no emit, no
// diagnostic). When the flag is on, the pass walks the TU's
// external_declarations, classifies definitions per the rules above,
// and emits the per-TU table + `[[gnu::constructor]]` registrar
// trio from spec § 4.1.
//
// The `gc_globals_warnf` helper is the warn-and-skip diagnostic-
// emission idiom (DF-001 / D-013). The shape mirrors
// `static_image_errorf` in xform_static_image.c (file:line 65-79)
// but at warning severity and without `exit()`, since D-009 says
// the build does not fail on the incomplete-struct case.
//
// ncc-internal vs emitted-C carve-out (n00b-api-guidelines.md):
//   * Code in this file is ncc-internal — uses libc, ncc_*
//     primitives, ncc_alloc_*, etc.
//   * The C source we *emit* via `ncc_buffer_printf` into the
//     post-transform TU is libn00b-flavored: uses `((void *)0)` not
//     `NULL` per D-016 (clangd test-file noise), `[[gnu::constructor]]`
//     not `__attribute__((constructor))`, and references
//     `n00b_gc_root_t` + `n00b_gc_register_roots` as defined in
//     libn00b's `core/gc.h` (D-005, D-012).

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "xform/transform.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_gc_globals.h"
#include "xform/xform_type_layout.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Diagnostic-emission idiom for warn-and-skip paths (DF-001 / D-013).
//
// Severity: warning. Build does NOT fail (no exit() call). Shape
// mirrors the dominant `ncc: <severity>: <fmt> (line %u, col %u)\n`
// idiom used by `static_image_errorf`, `gc_stack_errorf`, and
// `rpc_diagnostic`.
// ============================================================================

static void
gc_globals_warnf(ncc_parse_tree_t *node, const char *fmt, ...)
{
    uint32_t line = 0;
    uint32_t col  = 0;

    if (node) {
        ncc_xform_first_leaf_pos(node, &line, &col);
    }

    fprintf(stderr, "ncc: warning: ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (line %u, col %u)\n", line, col);
}

// ============================================================================
// TU-uniq derivation (unchanged from Phase 2)
// ============================================================================

static char *
tu_uniq_suffix(const char *input_file)
{
    const char *src = (input_file && input_file[0]) ? input_file
                                                     : "unknown_input";
    size_t        len = strlen(src);
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_') {
            ncc_buffer_putc(buf, (char)c);
        }
        else {
            ncc_buffer_putc(buf, '_');
        }
    }

    ncc_buffer_printf(buf, "_%zu", len);
    return ncc_buffer_take(buf);
}

// ============================================================================
// Generic vectors (ptr / parse-tree / string)
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
// Candidate entries collected during the TU walk. One per emitted
// table row (one struct decl may produce N candidates).
// ============================================================================

typedef enum {
    CAND_SCALAR, // `T *name;`
    CAND_ARRAY,  // `T *name[N];`
    CAND_STRUCT_RUN, // pointer-run inside a struct field
} cand_kind_t;

typedef struct {
    cand_kind_t kind;
    // For SCALAR / ARRAY: the variable name.
    // For STRUCT_RUN: the variable name (base of address arithmetic).
    char       *name;
    // For SCALAR: ignored (treated as 1).
    // For ARRAY:  the literal N.
    // For STRUCT_RUN: the run-length in pointer words.
    uint64_t    num_words;
    // For STRUCT_RUN only: heap-owned dotted field path that names
    // the first field of the run (e.g. "a", "in.p"). Used to build
    // the address-of expression `&(name).<field_path>`.
    char       *field_path;
} cand_t;

typedef struct {
    cand_t *items;
    size_t  len;
    size_t  cap;
} candvec_t;

static void
candvec_init(candvec_t *v)
{
    v->cap   = 8;
    v->len   = 0;
    v->items = ncc_alloc_array(cand_t, v->cap);
}

static void
candvec_push(candvec_t *v, cand_t c)
{
    if (v->len >= v->cap) {
        v->cap *= 2;
        v->items = ncc_realloc(v->items, v->cap * sizeof(cand_t));
    }
    v->items[v->len++] = c;
}

static void
candvec_free(candvec_t *v)
{
    for (size_t i = 0; i < v->len; i++) {
        ncc_free(v->items[i].name);
        ncc_free(v->items[i].field_path);
    }
    ncc_free(v->items);
}

// ============================================================================
// Common classification helpers (carry-overs from Phase 2 + new ones)
// ============================================================================

static bool
specs_contain_leaf_text(ncc_parse_tree_t *node, const char *text)
{
    if (!node) {
        return false;
    }

    if (ncc_tree_is_leaf(node)) {
        return ncc_xform_leaf_text_eq(node, text);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (specs_contain_leaf_text(ncc_tree_child(node, i), text)) {
            return true;
        }
    }
    return false;
}

static bool
declarator_has_outer_pointer(ncc_parse_tree_t *declarator)
{
    return ncc_xform_find_child_nt(declarator, "pointer") != nullptr;
}

// Returns true if `decl_specs` contain a `thread_local` (C23) or
// `_Thread_local` (C11) storage-class specifier. Such decls are not
// eligible for auto-registration: their address is not a compile-
// time constant (each thread gets its own copy), so the static-table
// emit path cannot reference them.
static bool
specs_contain_thread_local(ncc_parse_tree_t *decl_specs)
{
    return specs_contain_leaf_text(decl_specs, "thread_local")
           || specs_contain_leaf_text(decl_specs, "_Thread_local");
}

// Returns the top-level `atomic_type_specifier` node within
// `decl_specs`, or nullptr if the spec list does not use `_Atomic(T)`.
// Grammar: `atomic_type_specifier ::= _Atomic ( type_name )`. We
// only care about the type-specifier form (`_Atomic(T)`) — the
// type-qualifier form (`_Atomic T`) does not parse as an
// `atomic_type_specifier` and is handled correctly by the existing
// struct/scalar paths.
static ncc_parse_tree_t *
specs_atomic_type_specifier(ncc_parse_tree_t *decl_specs)
{
    return ncc_layout_first_descendant_nt(decl_specs,
                                          "atomic_type_specifier");
}

// Within an `atomic_type_specifier` (i.e., `_Atomic(T)`), return true
// iff the wrapped `type_name` has at least one outer `pointer` node in
// its abstract_declarator. That tells us `_Atomic(T *)` (or deeper)
// vs. `_Atomic(scalar)`.
static bool
atomic_type_specifier_wraps_pointer(ncc_parse_tree_t *atomic_spec)
{
    if (!atomic_spec) {
        return false;
    }
    ncc_parse_tree_t *type_name = ncc_xform_find_child_nt(atomic_spec,
                                                           "type_name");
    if (!type_name) {
        return false;
    }
    // The abstract_declarator (when present) carries the pointer.
    // A bare `type_name` of `T *` parses with a `pointer` somewhere
    // inside the abstract_declarator subtree.
    return ncc_layout_first_descendant_nt(type_name, "pointer") != nullptr;
}

static ncc_parse_tree_t *
declarator_direct(ncc_parse_tree_t *declarator)
{
    return ncc_xform_find_child_nt(declarator, "direct_declarator");
}

// Walk a `direct_declarator`'s first significant child (skipping
// `attribute_specifier_sequence` and group wrappers) and return what
// kind of declarator-shape it picks.
typedef enum {
    DD_IDENT,        // plain identifier (Phase 2 pointer-scalar shape)
    DD_ARRAY,        // direct_declarator → array_declarator
    DD_FUNCTION,     // direct_declarator → function_declarator
    DD_OTHER,        // parenthesized declarator, typedef name, etc.
} dd_shape_t;

static dd_shape_t
direct_declarator_shape(ncc_parse_tree_t *dd)
{
    if (!dd || ncc_tree_is_leaf(dd)) {
        return DD_OTHER;
    }

    size_t nc = ncc_tree_num_children(dd);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(dd, i);
        if (!c) {
            continue;
        }

        // Descend through alternation groups.
        while (c && !ncc_tree_is_leaf(c) && is_group_node(c)
               && ncc_tree_num_children(c) == 1) {
            c = ncc_tree_child(c, 0);
        }

        if (!c) {
            continue;
        }

        if (ncc_tree_is_leaf(c)) {
            continue;
        }

        if (ncc_xform_nt_name_is(c, "attribute_specifier_sequence")) {
            continue;
        }

        if (ncc_xform_nt_name_is(c, "identifier")
            || ncc_xform_nt_name_is(c, "synthetic_identifier")) {
            return DD_IDENT;
        }

        if (ncc_xform_nt_name_is(c, "array_declarator")) {
            return DD_ARRAY;
        }

        if (ncc_xform_nt_name_is(c, "function_declarator")) {
            return DD_FUNCTION;
        }

        return DD_OTHER;
    }

    return DD_OTHER;
}

// Extract the identifier name from a declarator. Walks the declarator's
// `direct_declarator` chain looking for a plain `identifier` or
// `synthetic_identifier` leaf. For arrays this descends through the
// `array_declarator → direct_declarator → identifier` path.
static char *
extract_name_from_declarator(ncc_parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    if (ncc_xform_nt_name_is(node, "identifier")
        || ncc_xform_nt_name_is(node, "synthetic_identifier")) {
        // Walk to first leaf.
        ncc_parse_tree_t *cur = node;
        while (!ncc_tree_is_leaf(cur) && ncc_tree_num_children(cur) > 0) {
            cur = ncc_tree_child(cur, 0);
        }
        if (cur && ncc_tree_is_leaf(cur)) {
            const char *text = ncc_xform_leaf_text(cur);
            if (text && *text) {
                return ncc_layout_copy_cstr(text);
            }
        }
        return nullptr;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *r = extract_name_from_declarator(ncc_tree_child(node, i));
        if (r) {
            return r;
        }
    }
    return nullptr;
}

static bool
name_is_ncc_internal(const char *name)
{
    return name && strncmp(name, "__ncc_", 6) == 0;
}

// Detects `const char` base type (`char const` ordering allowed).
static bool
base_type_is_const_char(ncc_parse_tree_t *decl_specs)
{
    char *bt = ncc_xform_collect_base_type(decl_specs);
    if (!bt) {
        return false;
    }

    size_t in_len = strlen(bt);
    char  *norm   = ncc_alloc_size(1, in_len + 1);
    size_t out    = 0;
    bool   ws     = false;
    bool   any    = false;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)bt[i];
        if (isspace(c)) {
            if (any && !ws) {
                norm[out++] = ' ';
                ws          = true;
            }
            continue;
        }
        norm[out++] = (char)c;
        any         = true;
        ws          = false;
    }
    if (out > 0 && norm[out - 1] == ' ') {
        out--;
    }
    norm[out] = '\0';
    ncc_free(bt);

    bool match = (strcmp(norm, "const char") == 0
                  || strcmp(norm, "char const") == 0);
    ncc_free(norm);
    return match;
}

// True iff the `init_declarator`'s initializer is a bare string
// literal (spec § 2.2 row 3).
static bool
initializer_is_string_literal(ncc_parse_tree_t *init_decl)
{
    ncc_parse_tree_t *init = ncc_xform_find_child_nt(init_decl,
                                                       "initializer");
    if (!init) {
        return false;
    }

    ncc_parse_tree_t *node = init;
    while (node && !ncc_tree_is_leaf(node)) {
        if (ncc_xform_nt_name_is(node, "string_literal")
            || ncc_xform_nt_name_is(node, "synthetic_string_literal")) {
            return true;
        }

        size_t nc = ncc_tree_num_children(node);
        if (nc != 1) {
            return false;
        }

        node = ncc_tree_child(node, 0);
    }

    if (!node || !ncc_tree_is_leaf(node)) {
        return false;
    }

    const char *text = ncc_xform_leaf_text(node);
    return text && text[0] == '"';
}

// ============================================================================
// `[[n00b::nomap]]` attribute detection + stripping
//
// Grammar: an attribute is an attribute_token (an identifier OR a
// `prefix :: identifier` pair) followed by an optional argument
// clause. The `n00b::nomap` form parses as an
// attribute_prefixed_token → `n00b :: nomap`.
//
// Detection: walk a subtree looking for an
// `attribute_prefixed_token` whose two relevant identifier leaves
// have text `n00b` and `nomap`. We perform this check on:
//   * the `declaration`'s prefix `attribute_specifier_sequence`
//   * each `init_declarator`'s trailing `attribute_specifier_sequence`
//   * any `attribute_specifier_sequence` nested inside the
//     declarator (e.g. between the type and the `=`).
// If any match, the decl is skipped. After detection we strip the
// matching `attribute_specifier` parent (the smallest containing
// `[[ ... ]]` block) so it does not appear in emitted C — clang must
// not see an unknown `n00b::nomap` attribute. If an `[[ ... ]]`
// block contains multiple attributes, we strip the entire block (an
// over-strip in a rare case the spec does not exercise; the n00b
// codebase does not currently combine `n00b::nomap` with other
// attributes in one `[[ ... ]]` block).
// ============================================================================

static bool
attribute_is_n00b_nomap(ncc_parse_tree_t *attr_node)
{
    if (!attr_node || ncc_tree_is_leaf(attr_node)) {
        return false;
    }

    // Look for an `attribute_prefixed_token` descendant whose two
    // identifier leaves are `n00b` and `nomap`.
    ncc_parse_tree_t *prefixed = ncc_layout_first_descendant_nt(
        attr_node, "attribute_prefixed_token");
    if (!prefixed) {
        return false;
    }

    // The token shape is: <attribute_prefix> "::" <identifier>.
    // attribute_prefix is itself an <identifier>. Collect the leaves
    // and compare.
    bool   saw_n00b  = false;
    bool   saw_nomap = false;
    size_t nc        = ncc_tree_num_children(prefixed);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(prefixed, i);
        if (!c) {
            continue;
        }
        // First the attribute_prefix wrapping the leading identifier;
        // then the trailing identifier. We accept any leaf-text path.
        ncc_parse_tree_t *cur = c;
        while (cur && !ncc_tree_is_leaf(cur) && ncc_tree_num_children(cur) > 0) {
            // Choose the first non-empty child until we hit a leaf.
            ncc_parse_tree_t *next = ncc_tree_child(cur, 0);
            if (!next) {
                break;
            }
            cur = next;
        }
        if (!cur || !ncc_tree_is_leaf(cur)) {
            continue;
        }
        const char *text = ncc_xform_leaf_text(cur);
        if (!text) {
            continue;
        }
        if (!saw_n00b && strcmp(text, "n00b") == 0) {
            saw_n00b = true;
            continue;
        }
        if (saw_n00b && !saw_nomap && strcmp(text, "nomap") == 0) {
            saw_nomap = true;
        }
    }

    return saw_n00b && saw_nomap;
}

// True iff `attr_seq_node` (an attribute_specifier_sequence) contains
// at least one `[[n00b::nomap]]` attribute.
static bool
attribute_seq_contains_nomap(ncc_parse_tree_t *attr_seq)
{
    if (!attr_seq || ncc_tree_is_leaf(attr_seq)) {
        return false;
    }

    // Collect all `attribute` nodes underneath.
    ncc_layout_parse_tree_list_t attrs = {0};
    ncc_layout_collect_nt_children(attr_seq, "attribute", &attrs);
    bool found = false;
    for (size_t i = 0; i < attrs.len; i++) {
        if (attribute_is_n00b_nomap(attrs.data[i])) {
            found = true;
            break;
        }
    }
    ncc_free(attrs.data);
    return found;
}

// True iff *any* attribute_specifier_sequence descendant of `node`
// carries `[[n00b::nomap]]`.
static bool
subtree_carries_nomap(ncc_parse_tree_t *node)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }

    if (ncc_xform_nt_name_is(node, "attribute_specifier_sequence")) {
        return attribute_seq_contains_nomap(node);
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_carries_nomap(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

// Remove every `attribute_specifier` block under `parent` that
// contains a `[[n00b::nomap]]` attribute. Operates in place.
// Spec § 3.3 also requires the attribute be stripped from non-
// qualifying positions; we strip unconditionally on the subtree
// passed in.
static void
strip_nomap_attribute_specifiers(ncc_parse_tree_t *parent)
{
    if (!parent || ncc_tree_is_leaf(parent)) {
        return;
    }

    size_t i = 0;
    while (i < ncc_tree_num_children(parent)) {
        ncc_parse_tree_t *child = ncc_tree_child(parent, i);
        if (child && !ncc_tree_is_leaf(child)
            && ncc_xform_nt_name_is(child, "attribute_specifier")
            && attribute_seq_contains_nomap(child)) {
            ncc_xform_remove_child(parent, i);
            continue;
        }

        strip_nomap_attribute_specifiers(child);
        i++;
    }
}

// ============================================================================
// Pointer-array classification (Phase 3)
//
// `T *x[N]`: outer pointer in declarator + direct_declarator is an
// `array_declarator`. We extract literal N via
// `ncc_layout_array_declarator_dimensions` (returns false on non-
// literal bounds, which we then skip without diagnostic — variable-
// length TU-scope arrays are not in v1).
//
// Arrays of function pointers (`void (*fns[N])(void);`) have NO
// outer pointer in the declarator (the `*` sits inside the
// parenthesized inner declarator under a function_declarator), so
// they fail the outer-pointer gate at the call site and never reach
// here.
// ============================================================================

static bool
classify_array_candidate(ncc_parse_tree_t *declarator, uint64_t *num_words_out)
{
    // Multi-dimensional arrays: collect all dimensions and multiply.
    // `ncc_layout_array_declarator_dimensions` returns false if any
    // bound is non-literal (variable-length, sizeof-style, etc.); we
    // skip such decls without warning per Phase 3 design (exotic in
    // a TU-scope pointer-array context).
    ncc_layout_uint64_list_t dims = {0};
    ncc_parse_tree_t        *bad  = nullptr;
    bool ok = ncc_layout_array_declarator_dimensions(declarator, &dims,
                                                      &bad);
    if (!ok) {
        ncc_free(dims.data);
        return false;
    }

    uint64_t prod = 1;
    for (size_t i = 0; i < dims.len; i++) {
        prod *= dims.data[i];
    }
    ncc_free(dims.data);

    if (prod == 0) {
        return false;
    }

    *num_words_out = prod;
    return true;
}

// ============================================================================
// Pointer-bearing struct classification (Phase 4)
//
// We walk struct members in source order using the same primitives
// `xform_gc_stack_maps.c` uses (member_declaration → declarator with
// pointer_depth_for_declarator). For each pointer-bearing slot, we
// append a (field_path, num_words) record to a `runs` list; adjacent
// pointer-bearing slots coalesce by extending the last record's
// run-length. A non-pointer-bearing slot (or a nested struct that
// itself produces zero pointer runs) breaks the run.
//
// Nested aggregate fields recursively call into the same walk with
// the field's path prefix. Nested arrays of aggregates iterate the
// array elements.
//
// The address-arithmetic emit uses `__builtin_offsetof(typeof(name),
// field_path)` — clang resolves `typeof(name)` to the actual struct
// type at the call site, which lets us address unnamed
// `struct { ... } name;` decls without ncc inventing a synthetic
// name.
// ============================================================================

typedef struct {
    char    *field_path; // dotted path, e.g. "a", "in.p", "v[0].p"
    uint64_t num_words;
} ptr_run_t;

typedef struct {
    ptr_run_t *items;
    size_t     len;
    size_t     cap;
    // True iff the last record in `items` is currently a live run we
    // can extend (no non-pointer break since the last push).
    bool       run_open;
} runvec_t;

static void
runvec_init(runvec_t *v)
{
    v->cap      = 4;
    v->len      = 0;
    v->items    = ncc_alloc_array(ptr_run_t, v->cap);
    v->run_open = false;
}

static void
runvec_push_pointer(runvec_t *v, const char *field_path, uint64_t num_words)
{
    if (v->run_open && v->len > 0) {
        // Extend last run.
        v->items[v->len - 1].num_words += num_words;
        return;
    }

    if (v->len >= v->cap) {
        v->cap *= 2;
        v->items = ncc_realloc(v->items, v->cap * sizeof(ptr_run_t));
    }

    v->items[v->len].field_path = ncc_layout_copy_cstr(field_path);
    v->items[v->len].num_words  = num_words;
    v->len++;
    v->run_open = true;
}

static void
runvec_break(runvec_t *v)
{
    v->run_open = false;
}

static void
runvec_free(runvec_t *v)
{
    for (size_t i = 0; i < v->len; i++) {
        ncc_free(v->items[i].field_path);
    }
    ncc_free(v->items);
}

// Forward declarations for the recursive struct-walk.
static bool walk_aggregate(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *agg_spec,
                           const char *path_prefix, runvec_t *out,
                           int depth, ncc_parse_tree_t *decl_node,
                           const char *decl_name);

static char *
append_path(const char *prefix, const char *field)
{
    if (prefix && *prefix) {
        return ncc_layout_format_cstr("%s.%s", prefix, field);
    }
    return ncc_layout_copy_cstr(field);
}

// Walk a member_declarator (per member_declaration_list entry).
// Returns true on success. Returns false on unresolved nested-struct
// types (caller surfaces D-009 warn-and-skip).
static bool
walk_member_declarator(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *member_specs,
                       ncc_parse_tree_t *member_declarator,
                       const char *path_prefix, runvec_t *out,
                       int depth, ncc_parse_tree_t *decl_node,
                       const char *decl_name)
{
    ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(member_declarator,
                                                           "declarator");
    if (!declarator) {
        // Bit-field-only member, etc.: not a pointer slot, break runs.
        runvec_break(out);
        return true;
    }

    char *field_name = ncc_layout_declarator_name(declarator);
    if (!field_name) {
        runvec_break(out);
        return true;
    }

    char *field_path = append_path(path_prefix, field_name);

    int ptr_depth = ncc_layout_pointer_depth_for_declarator(ctx, member_specs,
                                                            declarator);
    bool is_array = ncc_layout_first_descendant_nt(declarator,
                                                    "array_declarator")
                  != nullptr;

    if (ptr_depth > 0) {
        if (is_array) {
            // Pointer array as a struct field.
            ncc_layout_uint64_list_t dims = {0};
            ncc_parse_tree_t        *bad  = nullptr;
            if (ncc_layout_array_declarator_dimensions(declarator, &dims,
                                                       &bad)) {
                uint64_t prod = 1;
                for (size_t k = 0; k < dims.len; k++) {
                    prod *= dims.data[k];
                }
                ncc_free(dims.data);
                if (prod > 0) {
                    runvec_push_pointer(out, field_path, prod);
                }
                else {
                    runvec_break(out);
                }
            }
            else {
                // Non-literal bound — cannot enumerate; skip + break.
                ncc_free(dims.data);
                runvec_break(out);
            }
        }
        else {
            runvec_push_pointer(out, field_path, 1);
        }
        ncc_free(field_name);
        ncc_free(field_path);
        return true;
    }

    // Non-pointer slot. Could be a nested aggregate.
    ncc_layout_aggregate_type_info_t *nested_info
        = ncc_layout_aggregate_info_from_specs(ctx, member_specs);
    ncc_parse_tree_t *nested = nested_info
                                 ? nested_info->specifier
                                 : ncc_layout_aggregate_spec_from_specs(ctx,
                                                                        member_specs);

    if (!nested) {
        // Plain non-pointer-non-aggregate scalar field. Breaks run.
        runvec_break(out);
        ncc_free(field_name);
        ncc_free(field_path);
        return true;
    }

    if (is_array) {
        // Aggregate array field — iterate elements by index.
        ncc_layout_uint64_list_t dims = {0};
        ncc_parse_tree_t        *bad  = nullptr;
        if (!ncc_layout_array_declarator_dimensions(declarator, &dims, &bad)) {
            ncc_free(dims.data);
            // Cannot enumerate. Skip this field cleanly — but a
            // pointer-bearing aggregate with non-literal bounds in a
            // TU-scope decl is exotic. Break run, do not warn (the
            // decl itself is still candidate-eligible if other fields
            // are pointers).
            runvec_break(out);
            ncc_free(field_name);
            ncc_free(field_path);
            return true;
        }

        // Iterate the (possibly multi-dim) array linearly.
        uint64_t total = 1;
        for (size_t k = 0; k < dims.len; k++) {
            total *= dims.data[k];
        }

        // Flatten N-D to 1-D linear index, build `field[i1][i2]...`.
        for (uint64_t lin = 0; lin < total; lin++) {
            // Build subscript string `[i1][i2]...`.
            ncc_buffer_t *subscripts = ncc_buffer_empty();
            uint64_t rem = lin;
            for (size_t k = 0; k < dims.len; k++) {
                uint64_t suffix_prod = 1;
                for (size_t m = k + 1; m < dims.len; m++) {
                    suffix_prod *= dims.data[m];
                }
                uint64_t idx = rem / suffix_prod;
                rem          = rem % suffix_prod;
                ncc_buffer_printf(subscripts, "[%llu]",
                                  (unsigned long long)idx);
            }
            char *subs        = ncc_buffer_take(subscripts);
            char *elem_path   = ncc_layout_format_cstr("%s%s", field_path,
                                                       subs);
            bool  ok          = walk_aggregate(ctx, nested, elem_path, out,
                                                depth + 1, decl_node,
                                                decl_name);
            ncc_free(subs);
            ncc_free(elem_path);
            if (!ok) {
                ncc_free(dims.data);
                ncc_free(field_name);
                ncc_free(field_path);
                return false;
            }
        }

        ncc_free(dims.data);
        ncc_free(field_name);
        ncc_free(field_path);
        return true;
    }

    // Scalar nested aggregate.
    bool ok = walk_aggregate(ctx, nested, field_path, out, depth + 1,
                              decl_node, decl_name);
    ncc_free(field_name);
    ncc_free(field_path);
    return ok;
}

// Walk a member_declaration (one row in a struct's member list).
// Returns true on success, false on D-009 trigger (unresolved nested
// aggregate that prevents enumeration).
static bool
walk_member_declaration(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *member,
                        const char *path_prefix, runvec_t *out,
                        int depth, ncc_parse_tree_t *decl_node,
                        const char *decl_name)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        runvec_break(out);
        return true;
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");

    if (!member_list) {
        // Member with no member_declarator_list. This is one of three
        // cases:
        //   (a) C11 anonymous struct/union member:
        //         `struct { int x; };` or `union { ... };` — fields
        //         live at the parent's path with no extra subscript.
        //   (b) ncc grammar ambiguity quirk: simple scalar members
        //       like `int b;` sometimes parse with `b` absorbed into
        //       the specifier_qualifier_list as a typedef_name, and
        //       the member_declarator_list ends up empty. The right
        //       answer for v1 is "non-pointer-bearing slot": break
        //       the run, continue. (Matches the dominant call from
        //       xform_gc_stack_maps.c::expand_member_declaration
        //       file:line 2067 — it silently emits zero roots when
        //       the same shape appears.)
        //   (c) Same shape but with a pointer-typed
        //       specifier_qualifier_list. No field name available —
        //       cannot address — break run.
        int ptr_depth = ncc_layout_pointer_depth_for_specs(ctx, member_specs);
        if (ptr_depth > 0) {
            // No field name to address from — break run.
            runvec_break(out);
            return true;
        }

        ncc_layout_aggregate_type_info_t *aginfo
            = ncc_layout_aggregate_info_from_specs(ctx, member_specs);
        ncc_parse_tree_t *agg = aginfo
                                  ? aginfo->specifier
                                  : ncc_layout_aggregate_spec_from_specs(ctx,
                                                                         member_specs);

        if (!agg) {
            // Case (b): non-aggregate scalar (e.g. plain `int b;`).
            // Not a pointer, not an unresolved struct — just a
            // non-pointer-bearing slot. Break run, continue.
            runvec_break(out);
            return true;
        }

        // The anonymous aggregate's members live at the parent path.
        char *implicit_name = ncc_layout_implicit_member_field_name(
            member, member_specs);
        if (implicit_name) {
            // The member is an unnamed inline aggregate that is
            // bound to a field name (e.g. `struct { ... } in;`).
            char *child_path = append_path(path_prefix, implicit_name);
            bool  ok         = walk_aggregate(ctx, agg, child_path, out,
                                               depth + 1, decl_node,
                                               decl_name);
            ncc_free(child_path);
            ncc_free(implicit_name);
            return ok;
        }

        // Truly anonymous nested struct/union (no implicit name) —
        // fields are accessed directly on the parent in C, so we
        // recurse with the same path prefix.
        return walk_aggregate(ctx, agg, path_prefix, out, depth + 1,
                               decl_node, decl_name);
    }

    ncc_layout_parse_tree_list_t declarators = {0};
    ncc_layout_collect_nt_children(member_list, "member_declarator",
                                   &declarators);
    bool ok = true;
    for (size_t i = 0; i < declarators.len && ok; i++) {
        ok = walk_member_declarator(ctx, member_specs, declarators.data[i],
                                    path_prefix, out, depth, decl_node,
                                    decl_name);
    }
    ncc_free(declarators.data);
    return ok;
}

static bool
walk_aggregate(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *agg_spec,
               const char *path_prefix, runvec_t *out, int depth,
               ncc_parse_tree_t *decl_node, const char *decl_name)
{
    if (depth > 64) {
        // Pathological nesting; treat as unresolved.
        return false;
    }

    agg_spec = ncc_layout_resolve_aggregate_specifier(ctx, agg_spec);
    if (!agg_spec) {
        return false;
    }
    if (!ncc_layout_aggregate_specifier_has_members(agg_spec)) {
        return false;
    }

    bool is_union = ncc_layout_struct_or_union_is_union(agg_spec);

    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        agg_spec, "member_declaration_list");
    if (!members) {
        return false;
    }

    ncc_layout_parse_tree_list_t member_decls = {0};
    ncc_layout_collect_nt_children(members, "member_declaration",
                                   &member_decls);
    bool ok = true;
    for (size_t i = 0; i < member_decls.len && ok; i++) {
        ok = walk_member_declaration(ctx, member_decls.data[i],
                                     path_prefix, out, depth, decl_node,
                                     decl_name);
    }
    ncc_free(member_decls.data);

    if (is_union) {
        // Conservative coalescing within a union: a union arm that
        // is pointer-bearing keeps its slot open as a run-extender,
        // but the union as a whole breaks the parent run boundary
        // afterwards. Spec § 2.5 calls this out explicitly. v1's
        // n00b code has no TU-scope pointer-bearing unions; this
        // branch is here to keep the run semantics defined.
        runvec_break(out);
    }

    return ok;
}

// ============================================================================
// Per-declaration classification (called from the TU walk)
// ============================================================================

static void
classify_declaration(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *decl,
                     candvec_t *out_cands)
{
    // [[n00b::nomap]] attribute check (Phase 5).
    // Look at every attribute_specifier_sequence the parser stitched
    // onto the declaration. If any one contains [[n00b::nomap]], the
    // whole decl is silently skipped (no diagnostic, no entry).
    if (subtree_carries_nomap(decl)) {
        strip_nomap_attribute_specifiers(decl);
        return;
    }

    ncc_parse_tree_t *decl_specs
        = ncc_xform_find_child_nt(decl, "declaration_specifiers");
    if (!decl_specs) {
        return;
    }

    // Storage-class gate: extern / typedef do not define an object.
    if (specs_contain_leaf_text(decl_specs, "extern")) {
        return;
    }
    if (specs_contain_leaf_text(decl_specs, "typedef")) {
        return;
    }

    // Storage-class gate (F-2 / D-036): thread_local decls have
    // per-thread storage; their address is not a compile-time
    // constant, so a static `n00b_gc_root_t[]` table cannot embed
    // `&var`. clang rejects the emit; the only correct disposition
    // is to skip. Mirrors the `extern` skip above.
    if (specs_contain_thread_local(decl_specs)) {
        return;
    }

    ncc_parse_tree_t *init_list
        = ncc_xform_find_child_nt(decl, "init_declarator_list");
    if (!init_list) {
        // Decls like `struct foo;` (forward declaration of a tag) —
        // no object introduced, ignore.
        return;
    }

    bool const_char = base_type_is_const_char(decl_specs);

    // Atomic-type gate (F-3 / D-036): `_Atomic(T *)` globals are a
    // single opaque pointer word, NOT a struct whose fields can be
    // walked. The wrapper hides the pointed-to type from the field-
    // walking path; without this gate, the struct path would treat
    // the inner type as the variable's type and emit incorrect
    // per-field entries (or worse, recurse into an unrelated
    // struct). Disposition:
    //   * `_Atomic(T *)` (any pointer depth inside the wrapper):
    //     register as a scalar pointer with num_words = 1.
    //   * `_Atomic(scalar)` (non-pointer inside): not GC-relevant,
    //     silent skip.
    // Either way we bypass the struct/scalar branches below.
    ncc_parse_tree_t *atomic_spec = specs_atomic_type_specifier(decl_specs);
    bool atomic_wraps_ptr = atomic_spec
        ? atomic_type_specifier_wraps_pointer(atomic_spec) : false;

    // Is the base type a struct/union/typedef-resolved aggregate?
    // Used both for the "pointer-bearing struct" candidate path and
    // the D-009 unresolved-struct warn-and-skip.
    //
    // Suppressed when the decl uses `_Atomic(...)`: the inner type
    // is opaque to the GC and must not drive struct/aggregate
    // analysis (F-3 above).
    ncc_layout_aggregate_type_info_t *agg_info
        = atomic_spec
              ? nullptr
              : ncc_layout_aggregate_info_from_specs(ctx, decl_specs);
    ncc_parse_tree_t *agg = atomic_spec
                              ? nullptr
                              : (agg_info
                                     ? agg_info->specifier
                                     : ncc_layout_aggregate_spec_from_specs(
                                         ctx, decl_specs));

    // Detect whether the spec MENTIONS a struct/union keyword or a
    // typedef name that resolves to one. We use the same machinery
    // `xform_static_image.c` / `xform_gc_stack_maps.c` use, plus a
    // grammar check on `struct_or_union_specifier`.
    //
    // When the spec is `_Atomic(...)`, any `struct_or_union_specifier`
    // descendant lives INSIDE the wrapper and refers to the pointed-
    // to type, not the variable's type. Suppress the aggregate path
    // entirely in that case (F-3).
    bool specs_mention_aggregate
        = (!atomic_spec)
          && ncc_layout_first_descendant_nt(decl_specs,
                                            "struct_or_union_specifier")
                 != nullptr;

    size_t nc = ncc_tree_num_children(init_list);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(init_list, i);
        if (!c || ncc_tree_is_leaf(c)) {
            continue;
        }
        if (!ncc_xform_nt_name_is(c, "init_declarator")) {
            continue;
        }

        ncc_parse_tree_t *declarator
            = ncc_xform_find_child_nt(c, "declarator");
        if (!declarator) {
            continue;
        }

        ncc_parse_tree_t *dd = declarator_direct(declarator);
        dd_shape_t        shape = direct_declarator_shape(dd);

        bool has_outer_ptr = declarator_has_outer_pointer(declarator);

        // Function-declarator outer: function or function-pointer-
        // array. Either way: skip.
        if (shape == DD_FUNCTION) {
            continue;
        }

        // Atomic-wrapper path (F-3 / D-036). When the base type is
        // `_Atomic(...)`, the wrapper governs eligibility — not the
        // wrapped type. We never recurse into the wrapped type; the
        // atomic object is a single opaque word.
        //
        //   * `_Atomic(T *) x;`  → register as scalar pointer.
        //     (Arrays of atomic pointers, `_Atomic(T *) xs[N];`, are
        //     handled the same way the regular pointer-array shape
        //     would handle them, with shape == DD_ARRAY.)
        //   * `_Atomic(T) x;`    → skip (no GC-relevant pointer).
        //
        // Atomic-of-struct is intentionally not supported: the
        // wrapper makes the value opaque to the GC by design, and
        // there is no consumer in libn00b that needs scanning into a
        // struct held atomically as a value.
        if (atomic_spec) {
            if (!atomic_wraps_ptr) {
                continue;
            }
            if (shape != DD_IDENT && shape != DD_ARRAY) {
                continue;
            }
            char *name = extract_name_from_declarator(dd);
            if (!name) {
                continue;
            }
            if (name_is_ncc_internal(name)) {
                ncc_free(name);
                continue;
            }
            uint64_t n_words = 1;
            if (shape == DD_ARRAY) {
                if (!classify_array_candidate(declarator, &n_words)) {
                    ncc_free(name);
                    continue;
                }
            }
            cand_t cand = {
                .kind       = (shape == DD_ARRAY) ? CAND_ARRAY : CAND_SCALAR,
                .name       = name,
                .num_words  = n_words,
                .field_path = nullptr,
            };
            candvec_push(out_cands, cand);
            continue;
        }

        // Pointer-scalar (Phase 2): outer pointer + identifier
        // direct_declarator.
        if (has_outer_ptr && shape == DD_IDENT) {
            char *name = extract_name_from_declarator(dd);
            if (!name) {
                continue;
            }
            if (name_is_ncc_internal(name)) {
                ncc_free(name);
                continue;
            }
            if (const_char && initializer_is_string_literal(c)) {
                ncc_free(name);
                continue;
            }
            cand_t cand = {
                .kind       = CAND_SCALAR,
                .name       = name,
                .num_words  = 1,
                .field_path = nullptr,
            };
            candvec_push(out_cands, cand);
            continue;
        }

        // Pointer-array (Phase 3): outer pointer + array_declarator
        // direct_declarator.
        if (has_outer_ptr && shape == DD_ARRAY) {
            char *name = extract_name_from_declarator(dd);
            if (!name) {
                continue;
            }
            if (name_is_ncc_internal(name)) {
                ncc_free(name);
                continue;
            }
            uint64_t n = 0;
            if (!classify_array_candidate(declarator, &n)) {
                ncc_free(name);
                continue;
            }
            cand_t cand = {
                .kind       = CAND_ARRAY,
                .name       = name,
                .num_words  = n,
                .field_path = nullptr,
            };
            candvec_push(out_cands, cand);
            continue;
        }

        // Struct candidate (Phase 4): no outer pointer + identifier
        // direct_declarator + base type is a struct/union.
        if (!has_outer_ptr && shape == DD_IDENT
            && (agg || specs_mention_aggregate)) {
            char *name = extract_name_from_declarator(dd);
            if (!name) {
                continue;
            }
            if (name_is_ncc_internal(name)) {
                ncc_free(name);
                continue;
            }

            // If the aggregate is unresolved, that's the D-009 case.
            if (!agg || !ncc_layout_aggregate_specifier_has_members(agg)) {
                // Try to name the unresolved type for the warning.
                char *typename = nullptr;
                ncc_parse_tree_t *su = ncc_layout_first_descendant_nt(
                    decl_specs, "struct_or_union_specifier");
                if (su) {
                    typename = ncc_layout_aggregate_key_from_specifier(su);
                }
                if (!typename) {
                    typename = ncc_layout_first_typedef_name_text(decl_specs);
                }
                gc_globals_warnf(decl,
                                 "auto-gc-roots: cannot register '%s': "
                                 "type %s%s%s is incomplete; add "
                                 "[[n00b::nomap]] or include the full "
                                 "definition",
                                 name,
                                 typename ? "'" : "",
                                 typename ? typename : "<unresolved>",
                                 typename ? "'" : "");
                ncc_free(typename);
                ncc_free(name);
                continue;
            }

            // Walk the aggregate. Each pointer-bearing run becomes
            // one CAND_STRUCT_RUN entry.
            runvec_t runs;
            runvec_init(&runs);
            bool ok = walk_aggregate(ctx, agg, "", &runs, 0, decl, name);
            if (!ok) {
                gc_globals_warnf(decl,
                                 "auto-gc-roots: cannot register '%s': "
                                 "nested aggregate type is incomplete; "
                                 "add [[n00b::nomap]] or include the full "
                                 "definition",
                                 name);
                runvec_free(&runs);
                ncc_free(name);
                continue;
            }

            if (runs.len == 0) {
                // Aggregate has zero pointer-bearing fields — silent
                // skip (spec § 2.1 vs § 4.3).
                runvec_free(&runs);
                ncc_free(name);
                continue;
            }

            for (size_t r = 0; r < runs.len; r++) {
                cand_t cand = {
                    .kind       = CAND_STRUCT_RUN,
                    .name       = ncc_layout_copy_cstr(name),
                    .num_words  = runs.items[r].num_words,
                    .field_path = ncc_layout_copy_cstr(
                        runs.items[r].field_path),
                };
                candvec_push(out_cands, cand);
            }
            runvec_free(&runs);
            ncc_free(name);
            continue;
        }
    }
}

// ============================================================================
// Emit: build the forward-decl + table + ctor source string
// ============================================================================

static char *
build_emit_source(const char *tu_uniq, candvec_t *cands)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    // Forward declaration of the libn00b runtime symbol (D-012).
    ncc_buffer_puts(buf,
        "extern void n00b_gc_register_roots(const n00b_gc_root_t *roots,"
        " size_t count);\n");

    // Table.
    ncc_buffer_printf(buf,
        "static n00b_gc_root_t __ncc_gc_root_table_%s[] = {\n",
        tu_uniq);
    for (size_t i = 0; i < cands->len; i++) {
        cand_t *c = &cands->items[i];
        switch (c->kind) {
        case CAND_SCALAR:
            ncc_buffer_printf(buf,
                "    { .addr = (void *)&%s, .num_words = 1 },\n",
                c->name);
            break;
        case CAND_ARRAY:
            ncc_buffer_printf(buf,
                "    { .addr = (void *)&%s, .num_words = %llu },\n",
                c->name, (unsigned long long)c->num_words);
            break;
        case CAND_STRUCT_RUN:
            // Address of the first field of the run. ncc's grammar
            // restricts `__builtin_offsetof`'s second argument to a
            // single <identifier> (c_ncc.bnf line 599), so we cannot
            // round-trip a nested `outer.in.p` designator through
            // the post-transform reparse. Use `&(name).field.path`
            // directly — clang's unary `&` of a designated member
            // sub-expression yields the same byte address. The
            // run length (one entry covering N consecutive
            // pointer-words) is preserved in `.num_words`.
            ncc_buffer_printf(buf,
                "    { .addr = (void *)&(%s).%s, .num_words = %llu },\n",
                c->name, c->field_path,
                (unsigned long long)c->num_words);
            break;
        }
    }
    ncc_buffer_puts(buf, "};\n");

    // Constructor. Uses libn00b-flavored C — `[[gnu::constructor]]`
    // (C23 attribute spelling) and the single batch register call
    // (D-005).
    ncc_buffer_printf(buf,
        "[[gnu::constructor]]\n"
        "static void __ncc_gc_root_register_%s(void) {\n"
        "    n00b_gc_register_roots(__ncc_gc_root_table_%s,\n"
        "                           sizeof __ncc_gc_root_table_%s\n"
        "                               / sizeof __ncc_gc_root_table_%s[0]);\n"
        "}\n",
        tu_uniq, tu_uniq, tu_uniq, tu_uniq);

    return ncc_buffer_take(buf);
}

// ============================================================================
// Pass entry point (pre-order on translation_unit)
// ============================================================================

static ncc_parse_tree_t *
xform_gc_globals_tu(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *tu,
                    [[maybe_unused]] ncc_xform_control_t *control)
{
    ncc_xform_data_t *data = ncc_xform_get_data(ctx);

    // Cheapest possible no-op when the flag is off.
    if (!data || !data->auto_gc_roots) {
        return nullptr;
    }

    // Populate the layout caches (gc_aggregate_types,
    // gc_pointer_typedefs). Idempotent: `record_aggregate_type` /
    // `record_pointer_typedef` skip duplicates, so this is safe even
    // when xform_gc_stack_maps has already populated them in the
    // same ncc invocation.
    ncc_layout_collect_type_info(ctx, tu);

    // Flatten the existing TU into a list of external_declarations.
    ptrvec_t flat;
    ptrvec_init(&flat, 64);

    size_t tu_nc = ncc_tree_num_children(tu);
    for (size_t i = 0; i < tu_nc; i++) {
        flatten_group(ncc_tree_child(tu, i), &flat);
    }

    // Collect candidate entries in declaration order.
    candvec_t cands;
    candvec_init(&cands);

    for (size_t i = 0; i < flat.len; i++) {
        ncc_parse_tree_t *ext_decl = flat.items[i];
        if (!ext_decl || ncc_tree_is_leaf(ext_decl)) {
            continue;
        }

        ncc_parse_tree_t *inner    = nullptr;
        size_t            inner_nc = ncc_tree_num_children(ext_decl);
        for (size_t j = 0; j < inner_nc; j++) {
            ncc_parse_tree_t *c = ncc_tree_child(ext_decl, j);
            if (c && !ncc_tree_is_leaf(c)) {
                inner = c;
                break;
            }
        }
        if (!inner) {
            continue;
        }
        if (!ncc_xform_nt_name_is(inner, "declaration")) {
            continue;
        }

        classify_declaration(ctx, inner, &cands);
    }

    // After classification, strip any remaining `[[n00b::nomap]]`
    // attributes the parser stitched onto positions we did not
    // process via `classify_declaration` (spec § 3.3: silently
    // ignore at non-TU-scope decl positions but still strip so the
    // C compiler does not see them).
    strip_nomap_attribute_specifiers(tu);

    if (cands.len == 0) {
        // No qualifying decls → emit nothing. Per spec § 4.3.
        ncc_free(flat.items);
        candvec_free(&cands);
        return nullptr;
    }

    char *tu_uniq = tu_uniq_suffix(data->input_file);
    char *source  = build_emit_source(tu_uniq, &cands);
    ncc_free(tu_uniq);
    candvec_free(&cands);

    ncc_parse_tree_t *new_tu
        = ncc_xform_parse_source(ctx->grammar, "translation_unit",
                                 source, "xform_gc_globals");
    if (!new_tu) {
        fprintf(stderr,
                "ncc: error: failed to parse synthesized gc-globals "
                "registrar source:\n%s\n",
                source);
        ncc_free(source);
        ncc_free(flat.items);
        exit(1);
    }
    ncc_free(source);

    // Flatten the new TU into a list of external_declarations.
    ptrvec_t appended;
    ptrvec_init(&appended, 4);

    size_t new_nc = ncc_tree_num_children(new_tu);
    for (size_t i = 0; i < new_nc; i++) {
        flatten_group(ncc_tree_child(new_tu, i), &appended);
    }

    // Rebuild the TU: originals first, synthesized second, all
    // under one fresh `$$group_gc_globals` wrapper.
    ncc_nt_node_t gpn = {0};
    gpn.name          = ncc_string_from_cstr("$$group_gc_globals");
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
ncc_register_gc_globals_xform(ncc_xform_registry_t *reg)
{
    ncc_xform_register_pre(reg, "translation_unit", xform_gc_globals_tu,
                           "gc_globals");
}
