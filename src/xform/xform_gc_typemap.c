// xform_gc_typemap.c — WP-020 / D-049 link-time type->GC-map emission.
//
// For every `typehash(AGG *)` of a (single) pointer to a fully-defined
// aggregate, emit a precise per-element pointer-map descriptor, a pointer-
// bearing `n00b_gcmap` section entry keyed by that exact typehash, and a
// no-pointer `n00b_gcidx` placeholder. A post-link pass fills/sorts only
// `n00b_gcidx`; libn00b's `_n00b_alloc_raw` binary-searches that index to
// upgrade DEFAULT-scanned typed allocations to a precise CALLBACK scan, which
// makes them marshalable.
//
// Correctness: the walker is CONSERVATIVE. It emits a map only when it is
// certain it has found EVERY pointer word in the element type; on any
// uncertainty it marks the type unhandled (no entry → the allocation keeps its
// conservative DEFAULT scan, which is always GC-safe — never an UNDER-scan).
// A by-value (non-pointer) aggregate member must be a complete type in this TU
// (C rule), so ncc can always resolve it; a non-pointer member that does not
// resolve to a known aggregate is therefore a scalar with no pointer words.
//
// Offsets are emitted as `__builtin_offsetof` expressions: the C compiler does
// the arithmetic, so this never hand-computes a struct offset.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"
#include "xform/xform_gc_typemap.h"
#include "xform/xform_helpers.h"
#include "xform/xform_type_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Per-TU accumulator (ncc runs one translation unit per process invocation).
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t hash;
    char    *elem_type; // typedef/tag spelling used at the typehash site
} gc_typemap_record_t;

static gc_typemap_record_t *records   = nullptr;
static size_t               record_len = 0;
static size_t               record_cap = 0;

static bool
record_seen(uint64_t hash)
{
    for (size_t i = 0; i < record_len; i++) {
        if (records[i].hash == hash) {
            return true;
        }
    }
    return false;
}

static bool
elem_type_is_scalar_no_ptr(const char *elem_type)
{
    static const char *const scalar_types[] = {
        "_Bool",
        "bool",
        "char",
        "signed char",
        "unsigned char",
        "short",
        "short int",
        "signed short",
        "signed short int",
        "unsigned short",
        "unsigned short int",
        "int",
        "signed int",
        "unsigned int",
        "long",
        "long int",
        "signed long",
        "signed long int",
        "unsigned long",
        "unsigned long int",
        "long long",
        "long long int",
        "signed long long",
        "signed long long int",
        "unsigned long long",
        "unsigned long long int",
        "float",
        "double",
        "long double",
        "int8_t",
        "uint8_t",
        "int16_t",
        "uint16_t",
        "int32_t",
        "uint32_t",
        "int64_t",
        "uint64_t",
        "intptr_t",
        "uintptr_t",
        "ptrdiff_t",
        "size_t",
        "ssize_t",
        "n00b_size_t",
        "n00b_isize_t",
        "n00b_futex_t",
        "n00b_utf8_t",
        "n00b_codepoint_t",
    };

    if (!elem_type || strchr(elem_type, '*') != nullptr) {
        return false;
    }

    for (size_t i = 0; i < sizeof(scalar_types) / sizeof(scalar_types[0]); i++) {
        if (strcmp(elem_type, scalar_types[i]) == 0) {
            return true;
        }
    }

    return false;
}

static bool
aggregate_type_is_file_visible(ncc_layout_aggregate_type_info_t *info)
{
    if (!info || !info->specifier) {
        return false;
    }

    // The gcmap descriptors are appended at file scope after the transformed
    // translation unit. Block-local typedefs/tags are out of scope there, so
    // any descriptor that names them would make the generated C invalid.
    return ncc_xform_find_ancestor(info->specifier, "compound_statement") == nullptr
        && ncc_xform_find_ancestor(info->specifier, "function_definition") == nullptr;
}

// ---------------------------------------------------------------------------
// Field-path + offset-expression helpers.
// ---------------------------------------------------------------------------

static char *
path_join(const char *base, const char *name)
{
    if (base && *base) {
        return ncc_layout_format_cstr("%s.%s", base, name);
    }
    return ncc_layout_copy_cstr(name);
}

typedef struct {
    bool scan_kind;
    bool scan_cb;
    bool scan_user;
} runtime_scan_shape_t;

static bool
member_specs_typedef_is(ncc_parse_tree_t *member_specs, const char *expected)
{
    char *tdname = ncc_layout_first_typedef_name_text(member_specs);
    bool  result = tdname && strcmp(tdname, expected) == 0;
    if (tdname) {
        ncc_free(tdname);
    }
    return result;
}

static void
runtime_shape_note_member(ncc_xform_ctx_t      *ctx,
                          runtime_scan_shape_t *shape,
                          const char           *name,
                          ncc_parse_tree_t     *member_specs,
                          ncc_parse_tree_t     *declarator)
{
    if (!name) {
        return;
    }

    if (strcmp(name, "scan_kind") == 0) {
        shape->scan_kind = member_specs_typedef_is(member_specs,
                                                   "n00b_gc_scan_kind_t");
        return;
    }

    if (strcmp(name, "scan_cb") == 0) {
        shape->scan_cb = member_specs_typedef_is(member_specs,
                                                 "n00b_gc_scan_cb_t");
        return;
    }

    if (strcmp(name, "scan_user") == 0) {
        int ptr = declarator
                    ? ncc_layout_pointer_depth_for_declarator(ctx,
                                                              member_specs,
                                                              declarator)
                    : ncc_layout_pointer_depth_for_specs(ctx, member_specs);
        shape->scan_user = ptr > 0;
    }
}

static bool
aggregate_has_runtime_scan_shape(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *spec)
{
    runtime_scan_shape_t shape = {0};
    ncc_parse_tree_t    *members = ncc_xform_find_child_nt(
        spec, "member_declaration_list");
    if (!members) {
        return false;
    }

    ncc_layout_parse_tree_list_t mlist = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &mlist);

    for (size_t i = 0; i < mlist.len; i++) {
        ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
            mlist.data[i], "specifier_qualifier_list");
        ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
            mlist.data[i], "member_declarator_list");
        if (!member_specs) {
            continue;
        }

        if (!member_list) {
            if (ncc_layout_aggregate_spec_from_specs(ctx, member_specs)) {
                continue;
            }
            ncc_parse_tree_t *declarator = ncc_layout_first_descendant_nt(
                mlist.data[i], "declarator");
            char *name = ncc_layout_implicit_member_field_name(mlist.data[i],
                                                               member_specs);
            if (!name && declarator) {
                name = ncc_layout_declarator_name(declarator);
            }
            if (name) {
                runtime_shape_note_member(ctx, &shape, name, member_specs,
                                          declarator);
                ncc_free(name);
            }
            continue;
        }

        ncc_layout_parse_tree_list_t decls = {0};
        ncc_layout_collect_nt_children(member_list, "member_declarator",
                                       &decls);
        for (size_t j = 0; j < decls.len; j++) {
            ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
                decls.data[j], "declarator");
            if (!declarator) {
                continue;
            }
            char *name = ncc_layout_declarator_name(declarator);
            runtime_shape_note_member(ctx, &shape, name, member_specs,
                                      declarator);
            if (name) {
                ncc_free(name);
            }
        }
        if (decls.data) {
            ncc_free(decls.data);
        }
    }

    if (mlist.data) {
        ncc_free(mlist.data);
    }

    return shape.scan_kind && shape.scan_cb && shape.scan_user;
}

static bool
member_is_runtime_infra(bool runtime_scan_shape, const char *name,
                        ncc_parse_tree_t *member_specs)
{
    if (!name) {
        return false;
    }

    if (!runtime_scan_shape) {
        return false;
    }

    // These fields are runtime scan policy, not managed payload edges. Gate
    // this on the exact n00b scan triple so ordinary user fields named
    // `scan_user`, `allocator`, etc. are still walked normally.
    if (strcmp(name, "scan_user") == 0
        || (strcmp(name, "scan_cb") == 0
            && member_specs_typedef_is(member_specs, "n00b_gc_scan_cb_t"))) {
        return true;
    }

    char *tdname = ncc_layout_first_typedef_name_text(member_specs);
    bool  result = false;
    if (tdname) {
        result = (strcmp(name, "lock") == 0
                  && strcmp(tdname, "n00b_rwlock_t") == 0)
              || (strcmp(name, "allocator") == 0
                  && strcmp(tdname, "n00b_allocator_t") == 0);
        ncc_free(tdname);
    }

    return result;
}

static char *
offset_expr(const char *elem_type, const char *path)
{
    return ncc_layout_format_cstr("(__builtin_offsetof(%s,%s)/sizeof(void*))",
                                  elem_type, path);
}

static char *
offset_assert(const char *elem_type, const char *path)
{
    return ncc_layout_format_cstr(
        "static_assert((__builtin_offsetof(%s,%s)%%sizeof(void*))==0,"
        "\"n00b gc-map: pointer field must be word-aligned\");",
        elem_type, path);
}

// ---------------------------------------------------------------------------
// Tolerant, conservative pointer-offset walker.
// ---------------------------------------------------------------------------

static void
gc_walk(ncc_xform_ctx_t *ctx,
        const char      *elem_type,
        ncc_parse_tree_t *spec,
        const char      *base,
        ncc_buffer_t    *offs,
        ncc_buffer_t    *asserts,
        int             *count,
        bool            *ok,
        int              depth);

static void
emit_pointer(const char *elem_type, const char *path,
             ncc_buffer_t *offs, ncc_buffer_t *asserts, int *count)
{
    char *oe = offset_expr(elem_type, path);
    char *as = offset_assert(elem_type, path);
    if (*count > 0) {
        ncc_buffer_puts(offs, ",");
    }
    ncc_buffer_puts(offs, oe);
    ncc_buffer_puts(asserts, as);
    (*count)++;
    ncc_free(oe);
    ncc_free(as);
}

// Walk one member_declaration of a STRUCT.
static void
walk_struct_member(ncc_xform_ctx_t *ctx, const char *elem_type,
                   ncc_parse_tree_t *member, const char *base,
                   bool runtime_scan_shape, ncc_buffer_t *offs,
                   ncc_buffer_t *asserts, int *count, bool *ok, int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return; // nothing typed here (e.g. a stray ';')
    }

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");

    if (!member_list) {
        // Anonymous member: usually an anonymous aggregate (struct/union).
        // Resolve the aggregate FIRST and recurse — note that
        // pointer_depth_for_specs would return >0 for an anon aggregate whose
        // body contains pointer members (it sees the inner '*'), so the
        // aggregate check must come before any pointer test.
        ncc_parse_tree_t *nspec = ncc_layout_aggregate_spec_from_specs(
            ctx, member_specs);
        if (nspec) {
            char *field = ncc_layout_implicit_member_field_name(member,
                                                                member_specs);
            char *nbase = field ? path_join(base, field)
                                : ncc_layout_copy_cstr(base);
            gc_walk(ctx, elem_type, nspec, nbase, offs, asserts, count, ok,
                    depth + 1);
            ncc_free(nbase);
            if (field) {
                ncc_free(field);
            }
            return;
        }
        // Not an aggregate. In practice this is a typedef-to-pointer member
        // whose declarator the parser surfaced without a
        // member_declarator_list. If the typedef is a FUNCTION pointer
        // (n00b_gc_scan_cb_t / n00b_walk_action_t / n00b_hash_fn), EXCLUDE it:
        // it points to code, never the heap, so the GC must not scan it and
        // marshal must not relocate it. A DATA-pointer typedef must still be
        // MARKED (excluding it would under-scan and corrupt the heap).
        if (ncc_layout_pointer_depth_for_specs(ctx, member_specs) > 0) {
            char *tdname = ncc_layout_first_typedef_name_text(member_specs);
            bool  is_fnptr = tdname
                          && ncc_layout_typedef_name_is_function_pointer(ctx,
                                                                         tdname);
            if (tdname) {
                ncc_free(tdname);
            }
            if (is_fnptr) {
                return; // code pointer -> not a GC heap pointer
            }
            char *field = ncc_layout_implicit_member_field_name(member,
                                                                member_specs);
            if (field) {
                if (!member_is_runtime_infra(runtime_scan_shape, field,
                                             member_specs)) {
                    char *path = path_join(base, field);
                    emit_pointer(elem_type, path, offs, asserts, count);
                    ncc_free(path);
                }
                ncc_free(field);
            }
            else {
                *ok = false; // pointer member we cannot name
            }
        }
        return;
    }

    ncc_layout_parse_tree_list_t decls = {0};
    ncc_layout_collect_nt_children(member_list, "member_declarator", &decls);

    for (size_t i = 0; i < decls.len && *ok; i++) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(decls.data[i],
                                                              "declarator");
        if (!declarator) {
            continue;
        }
        char *name = ncc_layout_declarator_name(declarator);
        if (!name) {
            *ok = false; // can't address it -> can't reason about it
            break;
        }
        if (member_is_runtime_infra(runtime_scan_shape, name, member_specs)) {
            ncc_free(name);
            continue;
        }
        char *path     = path_join(base, name);
        bool  is_array = ncc_layout_first_descendant_nt(declarator,
                                                       "array_declarator")
                      != nullptr;
        int ptr = ncc_layout_pointer_depth_for_declarator(ctx, member_specs,
                                                          declarator);
        bool is_fnptr = ncc_layout_declarator_is_function_pointer(declarator);

        if (is_array) {
            // Data-pointer arrays / aggregate arrays are not handled in this
            // pass; scalar arrays and function-pointer arrays carry no GC heap
            // words and are safe to ignore.
            ncc_parse_tree_t *agg = ncc_layout_aggregate_spec_from_specs(
                ctx, member_specs);
            if ((ptr > 0 && !is_fnptr) || agg) {
                *ok = false;
            }
        }
        else if (ptr > 0) {
            if (!is_fnptr) {
                emit_pointer(elem_type, path, offs, asserts, count);
            }
        }
        else {
            // No '*' in the declarator, but the SPECS may still make this a
            // pointer: a pointer typedef, or an _Atomic(T *) wrapper. Detect
            // that before treating it as a nested by-value aggregate —
            // otherwise we would emit an offsetof THROUGH a pointer.
            int ptr_specs = ncc_layout_pointer_depth_for_specs(ctx,
                                                               member_specs);
            if (ptr_specs > 0) {
                char *tdname = ncc_layout_first_typedef_name_text(member_specs);
                bool  is_fnptr = tdname
                              && ncc_layout_typedef_name_is_function_pointer(
                                     ctx, tdname);
                if (tdname) {
                    ncc_free(tdname);
                }
                if (!is_fnptr) {
                    emit_pointer(elem_type, path, offs, asserts, count);
                }
                // function pointer -> excluded (code, not a heap pointer)
            }
            else {
                ncc_parse_tree_t *nspec = ncc_layout_aggregate_spec_from_specs(
                    ctx, member_specs);
                if (nspec) {
                    gc_walk(ctx, elem_type, nspec, path, offs, asserts, count,
                            ok, depth + 1);
                }
                // else: scalar by-value member -> no pointer words.
            }
        }

        ncc_free(path);
        ncc_free(name);
    }

    if (decls.data) {
        ncc_free(decls.data);
    }
}

// Classify + handle the members of a UNION. An all-pointer union occupies one
// word that is always a pointer (emit one offset); a union mixing pointer and
// non-pointer members can't be statically resolved (skip the type); an
// all-scalar union has no pointer words.
static void
walk_union(ncc_xform_ctx_t *ctx, const char *elem_type,
           ncc_parse_tree_t *spec, const char *base, bool runtime_scan_shape,
           ncc_buffer_t *offs, ncc_buffer_t *asserts, int *count, bool *ok)
{
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        spec, "member_declaration_list");
    if (!members) {
        *ok = false;
        return;
    }

    ncc_layout_parse_tree_list_t mlist = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &mlist);

    bool  any_scalar  = false;
    bool  any_complex = false;
    char *first_ptr   = nullptr;

    for (size_t i = 0; i < mlist.len; i++) {
        ncc_parse_tree_t *member       = mlist.data[i];
        ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
            member, "specifier_qualifier_list");
        ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
            member, "member_declarator_list");
        if (!member_specs) {
            continue;
        }

        if (!member_list) {
            // A union member with a single declarator may carry no
            // member_declarator_list wrapper (ncc puts the declarator directly
            // under the member_declaration). Classify it instead of treating
            // every such member as complex.
            //
            // Do not search through a by-value aggregate member's body for a
            // declarator: that would mistake fields inside `struct { ... } x`
            // for direct union alternatives and emit invalid paths like
            // `offsetof(outer, inner_field)`.
            if (ncc_layout_aggregate_spec_from_specs(ctx, member_specs)) {
                any_complex = true;
                continue;
            }
            ncc_parse_tree_t *d = ncc_layout_first_descendant_nt(member,
                                                                 "declarator");
            if (!d) {
                // No declarator: either a scalar member whose name isn't
                // wrapped in a declarator (e.g. `long x;`) or a truly
                // anonymous nested aggregate. Aggregate -> complex; otherwise
                // it's a scalar (no pointer words).
                any_scalar = true;
                continue;
            }
            bool is_array = ncc_layout_first_descendant_nt(d, "array_declarator")
                         != nullptr;
            int  p = ncc_layout_pointer_depth_in_declarator(d)
                   + ncc_layout_pointer_depth_for_specs(ctx, member_specs);
            bool is_fnptr = ncc_layout_declarator_is_function_pointer(d);
            if (is_array) {
                if (is_fnptr) {
                    any_scalar = true;
                }
                else {
                    any_complex = true;
                }
            }
            else if (p > 0) {
                if (is_fnptr) {
                    any_scalar = true;
                }
                else if (!first_ptr) {
                    char *nm = ncc_layout_declarator_name(d);
                    if (nm) {
                        first_ptr = path_join(base, nm);
                        ncc_free(nm);
                    }
                    else {
                        any_complex = true; // pointer we can't name
                    }
                }
            }
            else {
                any_scalar = true;
            }
            continue;
        }

        ncc_layout_parse_tree_list_t decls = {0};
        ncc_layout_collect_nt_children(member_list, "member_declarator",
                                       &decls);
        for (size_t j = 0; j < decls.len; j++) {
            ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(
                decls.data[j], "declarator");
            if (!declarator) {
                continue;
            }
            char *name = ncc_layout_declarator_name(declarator);
            if (!name) {
                any_complex = true;
                continue;
            }
            if (member_is_runtime_infra(runtime_scan_shape, name,
                                        member_specs)) {
                ncc_free(name);
                continue;
            }
            bool is_array = ncc_layout_first_descendant_nt(declarator,
                                                          "array_declarator")
                         != nullptr;
            int ptr = ncc_layout_pointer_depth_for_declarator(
                ctx, member_specs, declarator);
            bool is_fnptr =
                ncc_layout_declarator_is_function_pointer(declarator);
            if (is_array) {
                if (is_fnptr) {
                    any_scalar = true;
                }
                else {
                    any_complex = true;
                }
            }
            else if (ptr > 0) {
                if (is_fnptr) {
                    any_scalar = true;
                }
                else if (!first_ptr) {
                    first_ptr = path_join(base, name);
                }
            }
            else {
                ncc_parse_tree_t *agg = ncc_layout_aggregate_spec_from_specs(
                    ctx, member_specs);
                if (agg) {
                    any_complex = true;
                }
                else {
                    any_scalar = true;
                }
            }
            ncc_free(name);
        }
        if (decls.data) {
            ncc_free(decls.data);
        }
    }

    if (mlist.data) {
        ncc_free(mlist.data);
    }

    if (any_complex || (first_ptr && any_scalar)) {
        // The union can't be statically described as all-pointer or
        // all-scalar, so the whole type gets NO precise GC pointer-map and
        // silently falls back to conservative scanning. That conservative
        // scan can misread a scalar union payload (e.g. a packed {min,max}
        // like 0x100000000) as a pointer, which makes the type
        // unmarshalable at runtime. Warn (don't fail) so the build still
        // completes but every offending type is surfaced; the fix is to
        // split the union into separate pointer / non-pointer fields.
        fprintf(stderr,
                "ncc: warning: gc-typemap: type '%s' has a union that cannot "
                "be statically described for GC scanning (%s); the type loses "
                "its precise pointer map and falls back to conservative scan "
                "(not precisely marshalable). Split the union into separate "
                "pointer and non-pointer fields.\n",
                elem_type ? elem_type : "(anonymous)",
                any_complex
                    ? "a member is a by-value aggregate, an unnamed pointer, "
                      "or a non-function-pointer array"
                    : "it mixes pointer and non-pointer members");
        *ok = false; // can't statically describe this union
    }
    else if (first_ptr && !any_scalar) {
        emit_pointer(elem_type, first_ptr, offs, asserts, count); // all-pointer
    }
    // else: all-scalar union -> nothing.

    if (first_ptr) {
        ncc_free(first_ptr);
    }
}

// Safety cap on a single type's pointer-offset count. Real n00b types have a
// few dozen pointer words; a blow-up past this means the walk recursed into
// something pathological (a huge/wide nested expansion) — bail the type rather
// than emit a giant generated file that overwhelms the C compiler.
#define NCC_GCMAP_MAX_OFFSETS 256

static void
gc_walk(ncc_xform_ctx_t *ctx, const char *elem_type, ncc_parse_tree_t *spec,
        const char *base, ncc_buffer_t *offs, ncc_buffer_t *asserts,
        int *count, bool *ok, int depth)
{
    if (depth > 64 || *count > NCC_GCMAP_MAX_OFFSETS) {
        *ok = false;
        return;
    }

    spec = ncc_layout_resolve_aggregate_specifier(ctx, spec);
    if (!spec) {
        *ok = false;
        return;
    }

    bool runtime_scan_shape = aggregate_has_runtime_scan_shape(ctx, spec);

    if (ncc_layout_struct_or_union_is_union(spec)) {
        walk_union(ctx, elem_type, spec, base, runtime_scan_shape, offs, asserts,
                   count, ok);
        return;
    }

    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        spec, "member_declaration_list");
    if (!members) {
        *ok = false; // unresolved/incomplete aggregate
        return;
    }

    ncc_layout_parse_tree_list_t mlist = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &mlist);
    for (size_t i = 0; i < mlist.len && *ok; i++) {
        walk_struct_member(ctx, elem_type, mlist.data[i], base,
                           runtime_scan_shape, offs, asserts, count, ok,
                           depth);
    }
    if (mlist.data) {
        ncc_free(mlist.data);
    }
}

// ---------------------------------------------------------------------------
// Recording (called from xform_typehash).
// ---------------------------------------------------------------------------

void
ncc_gc_typemap_note(ncc_xform_ctx_t *ctx, const char *type_str, uint64_t hash)
{
    if (!ctx || !type_str || !hash) {
        return;
    }

    // _Atomic(T *) may carry a trailing '*' in its normalized spelling, but it
    // is not an aggregate object type we can allocate and describe with
    // offsetof(). Atomic pointer FIELDS are handled by gc_walk; top-level
    // atomic-pointer typehash records are not GC-map descriptor candidates.
    if (strstr(type_str, "_Atomic") != nullptr) {
        return;
    }

    // Must be a single pointer: exactly one trailing '*', no '*'/'[' before it.
    size_t len = strlen(type_str);
    while (len > 0 && (type_str[len - 1] == ' ' || type_str[len - 1] == '\t')) {
        len--;
    }
    if (len < 2 || type_str[len - 1] != '*') {
        return;
    }
    size_t base_len = len - 1;
    for (size_t i = 0; i < base_len; i++) {
        if (type_str[i] == '*' || type_str[i] == '[') {
            return; // multi-level pointer / array of pointers
        }
    }
    // Trim the element-type spelling.
    while (base_len > 0
           && (type_str[base_len - 1] == ' ' || type_str[base_len - 1] == '\t')) {
        base_len--;
    }
    if (base_len == 0) {
        return;
    }

    char *elem = ncc_alloc_array(char, base_len + 1);
    memcpy(elem, type_str, base_len);
    elem[base_len] = '\0';

    // Aggregate resolution is deferred to emit() (which runs after all
    // xforms, when the aggregate table is fully populated). Here we only
    // record candidate single-pointer element types; non-aggregates and
    // unresolved types are filtered out at emit time. Skip obvious
    // non-aggregates to keep the record set small.
    if (strcmp(elem, "void") == 0) {
        ncc_free(elem);
        return;
    }

    if (record_seen(hash)) {
        ncc_free(elem);
        return;
    }

    if (record_len == record_cap) {
        size_t ncap = record_cap ? record_cap * 2 : 16;
        records = (gc_typemap_record_t *)ncc_realloc(
            records, ncap * sizeof(gc_typemap_record_t));
        record_cap = ncap;
    }
    records[record_len].hash      = hash;
    records[record_len].elem_type = elem;
    record_len++;
}

// ---------------------------------------------------------------------------
// Emission (called from ncc.c after ncc_xform_apply).
// ---------------------------------------------------------------------------

// ncc emits POST-preprocessed C, so the `N00B_GC_TYPE_MAP_*_SECTION` macros
// would not expand in the appended code. Derive the platform-correct section
// name from n00b's already-expanded static-object attribute, but always spell
// the generated GC-map attribute in C23 form.
static char *
derive_gc_section_attr(const char *stobj_attr, const char *section_name)
{
    if (stobj_attr) {
        const char *p = strstr(stobj_attr, "n00b_stobj");
        if (p) {
            const char *start = p;
            while (start > stobj_attr && start[-1] != '"') {
                start--;
            }
            const char *end = p + strlen("n00b_stobj");
            while (*end && *end != '"') {
                end++;
            }
            return ncc_layout_format_cstr(
                "[[gnu::section(\"%.*s%s%.*s\"), gnu::used]]",
                (int)(p - start), start, section_name,
                (int)(end - (p + strlen("n00b_stobj"))),
                p + strlen("n00b_stobj"));
        }
    }
    return ncc_layout_format_cstr("[[gnu::section(\"%s\"), gnu::used]]",
                                  section_name);
}

static char *
derive_gcmap_attr(const char *stobj_attr)
{
    return derive_gc_section_attr(stobj_attr, "n00b_gcmap");
}

static char *
derive_gcidx_attr(const char *stobj_attr)
{
    return derive_gc_section_attr(stobj_attr, "n00b_gcidx");
}

char *
ncc_gc_typemap_emit(ncc_xform_ctx_t *ctx)
{
    ncc_buffer_t *out      = ncc_buffer_empty();
    size_t        n_emitted = 0;
    const char   *stobj_attr = ncc_xform_get_data(ctx)->static_object_entry_attr;
    char         *gcmap_attr = derive_gcmap_attr(stobj_attr);
    char         *gcidx_attr = derive_gcidx_attr(stobj_attr);

    for (size_t i = 0; i < record_len; i++) {
        // Skip only source-level generic macro spellings such as
        // `n00b_dict_t(...)`: ncc appends this C after preprocessing, so those
        // macros no longer exist. Resolved generated tags (`struct __...`) are
        // real aggregate names in the post-preprocessed TU and are exactly what
        // typehash sites for generic containers often use; they must be
        // eligible so dict/list wrapper objects get precise maps instead of
        // DEFAULT-scanning function pointers and scan infrastructure fields.
        if (strchr(records[i].elem_type, '(') != nullptr) {
            continue;
        }

        bool scalar_no_ptr = elem_type_is_scalar_no_ptr(records[i].elem_type);
        ncc_layout_aggregate_type_info_t *info = nullptr;
        if (!scalar_no_ptr) {
            info = ncc_layout_aggregate_info_from_type_name(ctx,
                                                            records[i].elem_type);
            if (!info || !info->specifier) {
                continue;
            }
            if (!aggregate_type_is_file_visible(info)) {
                continue;
            }
            if (info->is_atomic) {
                continue;
            }
        }

        ncc_buffer_t *offs    = ncc_buffer_empty();
        ncc_buffer_t *asserts = ncc_buffer_empty();
        int           count   = 0;
        bool          ok      = true;

        if (!scalar_no_ptr) {
            gc_walk(ctx, records[i].elem_type, info->specifier, "", offs, asserts,
                    &count, &ok, 0);
        }

        if (ok) {
            char *asserts_str = ncc_buffer_take(asserts);

            // Asserts first (file scope), then the descriptor + section entry.
            ncc_buffer_printf(out, "%s", asserts_str);
            if (count > 0) {
                char *offs_csv = ncc_buffer_take(offs);
                ncc_buffer_printf(
                    out,
                    "static const uint64_t __ncc_gcmap_off_%zu[]={%s};",
                    i, offs_csv);
                ncc_free(offs_csv);
            }
            else {
                ncc_free(ncc_buffer_take(offs));
            }
            char *offsets_expr = count > 0
                               ? ncc_layout_format_cstr("__ncc_gcmap_off_%zu", i)
                               : ncc_layout_copy_cstr("nullptr");
            ncc_buffer_printf(
                out,
                "static const n00b_gc_struct_layout_t __ncc_gcmap_lay_%zu="
                "{.stride=(sizeof(%s)/sizeof(void*)),.count=1,.offset_count=%d,"
                ".offsets=%s};",
                i, records[i].elem_type, count, offsets_expr);
            ncc_buffer_printf(
                out,
                "%s static const n00b_gc_type_map_entry_t "
                "__ncc_gcmap_ent_%zu={.type_hash=%lluULL,.layout="
                "&__ncc_gcmap_lay_%zu};",
                gcmap_attr, i, (unsigned long long)records[i].hash, i);
            ncc_buffer_printf(
                out,
                "%s static const n00b_gc_type_map_index_entry_t "
                "__ncc_gcidx_ent_%zu={.type_hash=0ULL,.entry_index=%zuULL};",
                gcidx_attr, i, i);
            n_emitted++;

            ncc_free(offsets_expr);
            ncc_free(asserts_str);
        }
        else {
            ncc_free(ncc_buffer_take(offs));
            ncc_free(ncc_buffer_take(asserts));
        }
    }

    ncc_free(gcmap_attr);
    ncc_free(gcidx_attr);

    // Drain the accumulator (per-TU state).
    for (size_t i = 0; i < record_len; i++) {
        ncc_free(records[i].elem_type);
    }
    ncc_free(records);
    records    = nullptr;
    record_len = 0;
    record_cap = 0;

    if (n_emitted == 0) {
        ncc_free(ncc_buffer_take(out));
        return ncc_layout_copy_cstr("");
    }

    return ncc_buffer_take(out);
}
