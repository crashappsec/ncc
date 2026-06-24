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
#include <stdlib.h>
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
// Discriminated unions (n00b_variant_t).
//
// `n00b_variant_t(T1, T2, ...)` expands to a struct shaped exactly:
//     struct { uint64_t selector; union { T1 field_<h1>; ... } value; }
// where `selector` holds typehash(T) of the active alternative (0 = unset).
// The conservative union walker cannot describe this, but the discriminant
// makes it precisely scannable: the `value` word is a heap pointer iff the
// selector equals the typehash of one of the POINTER alternatives. ncc records
// those typehashes (recomputed exactly as `typehash(T)` does, so they match the
// runtime selector byte-for-byte) and emits a variant descriptor the collector
// resolves per element at scan time.
// ---------------------------------------------------------------------------

// Accumulates the emitted variant descriptors for one element type.
typedef struct {
    ncc_buffer_t *arrays; // file-scope ptr-hash arrays + offset asserts
    ncc_buffer_t *inits;  // CSV of n00b_gc_variant_field_t initializers
    int           count;  // number of variant fields recorded
    size_t        rec;    // owning record index (for unique array names)
} variant_acc_t;

typedef enum {
    ALT_PTR,        // a heap pointer; its typehash joins the discriminant set
    ALT_NONPTR,     // carries no heap pointer (primitive scalar or code pointer)
    ALT_AGGREGATE,  // a by-value aggregate; its pointer offsets are walked per-arm
    ALT_UNSUPPORTED // cannot be described safely -> fall back to conservative
} alt_class_t;

// Normalized type spelling of a struct member, with the trailing field name
// removed. A member without a pointer/array declarator (e.g. `uint64_t x;`) is
// parsed ambiguously: the field name folds into the specifier-qualifier-list,
// so `normalize(specs)` yields "uint64_t x". Because the canonicalizer
// space-joins adjacent alnum tokens, the field name is exactly a trailing
// " <name>" we can strip — leaving normalize(T), which equals what
// `typehash(T)` hashes at the n00b_variant_set site. Caller frees.
static char *
member_type_spelling(ncc_parse_tree_t *specs, const char *field_name)
{
    ncc_string_t norm = ncc_normalize_type_tree(specs);
    if (!norm.data) {
        return nullptr;
    }
    if (field_name) {
        size_t blen = strlen(norm.data);
        size_t flen = strlen(field_name);
        if (blen > flen + 1 && norm.data[blen - flen - 1] == ' '
            && strcmp(norm.data + blen - flen, field_name) == 0) {
            norm.data[blen - flen - 1] = '\0';
        }
    }
    return norm.data;
}

// True if any leaf under `node` is a const/volatile qualifier. A variant
// alternative carrying such a qualifier on its pointer (e.g. `T * const`) would
// change typehash normalization, so we cannot reconstruct a matching selector
// value and must fall back to a conservative scan.
static bool
subtree_has_cv_qualifier(ncc_parse_tree_t *node)
{
    if (!node) {
        return false;
    }
    if (ncc_tree_is_leaf(node)) {
        const char *t = ncc_xform_leaf_text(node);
        return t && (strcmp(t, "const") == 0 || strcmp(t, "volatile") == 0);
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        if (subtree_has_cv_qualifier(ncc_tree_child(node, i))) {
            return true;
        }
    }
    return false;
}

// Classify one alternative (a member_declaration of the value union) and, for a
// pointer alternative, compute its typehash into *out_hash. The hash MUST equal
// `typehash(T)` at the n00b_variant_set site: the runtime selector is that
// value, and a mismatch would leave a live pointer unmarked (heap corruption).
// We reproduce it exactly — normalize(specifier_qualifier_list) yields the
// canonical base, and the canonicalizer appends pointer stars with no spacing,
// so base + "*"*depth == normalize(T).
static alt_class_t
classify_variant_alt(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *member,
                     uint64_t *out_hash)
{
    ncc_parse_tree_t *specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!specs) {
        return ALT_UNSUPPORTED;
    }
    // _Atomic is dropped by normalization, but treat atomic alternatives as
    // unsupported rather than reason about the wrapper here.
    if (ncc_layout_specs_have_atomic_type_wrapper(specs)) {
        return ALT_UNSUPPORTED;
    }

    // A non-pointer scalar member (e.g. `uint64_t field_1;`) carries no
    // declarator node, so the declarator is optional here.
    ncc_parse_tree_t *decl = ncc_layout_first_descendant_nt(member,
                                                            "declarator");
    if (decl && ncc_layout_first_descendant_nt(decl, "array_declarator")) {
        return ALT_UNSUPPORTED;
    }

    int total = decl ? ncc_layout_pointer_depth_for_declarator(ctx, specs, decl)
                     : ncc_layout_pointer_depth_for_specs(ctx, specs);

    char *field = ncc_layout_implicit_member_field_name(member, specs);
    char *base  = member_type_spelling(specs, field);
    if (field) {
        ncc_free(field);
    }
    if (!base) {
        return ALT_UNSUPPORTED;
    }

    if (total == 0) {
        // Non-pointer by value: a recognized primitive scalar carries no heap
        // word. A by-value aggregate is described precisely per-arm by
        // walk_variant (which walks its pointer fields via gc_walk); its
        // selector is typehash(T) of the aggregate spelling — the same value
        // n00b_variant_set stamps. An unknown typedef also reaches here and is
        // tagged ALT_AGGREGATE; walk_variant then fails to resolve an aggregate
        // spec for it and falls back to conservative (never an under-scan).
        bool scalar = elem_type_is_scalar_no_ptr(base);
        if (scalar) {
            ncc_free(base);
            return ALT_NONPTR;
        }
        *out_hash = ncc_type_hash_u64(base);
        ncc_free(base);
        return ALT_AGGREGATE;
    }

    if (decl && ncc_layout_declarator_is_function_pointer(decl)) {
        ncc_free(base);
        return ALT_NONPTR; // code pointer, never a heap pointer
    }
    if (decl && subtree_has_cv_qualifier(decl)) {
        ncc_free(base);
        return ALT_UNSUPPORTED; // qualifier would break typehash reconstruction
    }

    // Reproduce typehash(T) exactly: the cleaned spelling is normalize(T) for
    // the spec-level type, and we append only the EXPLICIT declarator stars (a
    // pointer typedef is already captured by the spelling). The canonicalizer
    // never spaces before '*', so this equals normalize(T).
    int           stars = decl ? ncc_layout_pointer_depth_in_declarator(decl) : 0;
    ncc_buffer_t *tb    = ncc_buffer_empty();
    ncc_buffer_puts(tb, base);
    for (int i = 0; i < stars; i++) {
        ncc_buffer_puts(tb, "*");
    }
    *out_hash = ncc_type_hash_u64(tb->data);
    ncc_free(base);
    ncc_free(ncc_buffer_take(tb));
    return ALT_PTR;
}

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

// If `spec` is an n00b_variant_t struct, return its resolved value-union
// specifier; otherwise nullptr. The shape is exact: a struct with precisely two
// members — `uint64_t selector;` and `union { ... } value;` whose alternatives
// are all named with the `field_` prefix (N00B_VARIANT_FIELD). That triple
// signature is unique to n00b_variant_t, so the selector is guaranteed to hold
// a typehash discriminant.
static ncc_parse_tree_t *
variant_value_union(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *spec)
{
    if (ncc_layout_struct_or_union_is_union(spec)) {
        return nullptr; // a variant is a struct, not a union
    }
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        spec, "member_declaration_list");
    if (!members) {
        return nullptr;
    }

    ncc_layout_parse_tree_list_t mlist = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &mlist);
    if (mlist.len != 2) {
        if (mlist.data) {
            ncc_free(mlist.data);
        }
        return nullptr;
    }

    bool              have_selector = false;
    ncc_parse_tree_t *value_union   = nullptr;

    for (size_t i = 0; i < mlist.len; i++) {
        ncc_parse_tree_t *member = mlist.data[i];
        ncc_parse_tree_t *specs  = ncc_xform_find_child_nt(
            member, "specifier_qualifier_list");
        if (!specs) {
            continue;
        }
        char *name = ncc_layout_implicit_member_field_name(member, specs);
        if (!name) {
            continue;
        }

        if (strcmp(name, "selector") == 0) {
            char *base    = member_type_spelling(specs, name);
            have_selector = base && strcmp(base, "uint64_t") == 0;
            if (base) {
                ncc_free(base);
            }
        }
        else if (strcmp(name, "value") == 0) {
            ncc_parse_tree_t *agg = ncc_layout_aggregate_spec_from_specs(ctx,
                                                                         specs);
            if (agg) {
                ncc_parse_tree_t *u = ncc_layout_resolve_aggregate_specifier(
                    ctx, agg);
                if (u && ncc_layout_struct_or_union_is_union(u)) {
                    value_union = u;
                }
            }
        }
        ncc_free(name);
    }

    if (mlist.data) {
        ncc_free(mlist.data);
    }

    if (!have_selector || !value_union) {
        return nullptr;
    }

    // Confirm the n00b_variant alternative-naming convention: every union
    // member is named `field_...`. This rules out an ordinary 2-member struct
    // that merely happens to pair a uint64 with a union.
    ncc_parse_tree_t *umembers = ncc_xform_find_child_nt(
        value_union, "member_declaration_list");
    if (!umembers) {
        return nullptr;
    }
    ncc_layout_parse_tree_list_t alts = {0};
    ncc_layout_collect_nt_children(umembers, "member_declaration", &alts);
    bool all_field = alts.len > 0;
    for (size_t i = 0; i < alts.len; i++) {
        ncc_parse_tree_t *aspecs = ncc_xform_find_child_nt(
            alts.data[i], "specifier_qualifier_list");
        char *name = ncc_layout_implicit_member_field_name(alts.data[i], aspecs);
        if (!name || strncmp(name, "field_", 6) != 0) {
            all_field = false;
        }
        if (name) {
            ncc_free(name);
        }
    }
    if (alts.data) {
        ncc_free(alts.data);
    }

    return all_field ? value_union : nullptr;
}

// Forward declarations: walk_variant (below) drives these, which are defined
// later in the file.
static void
emit_pointer(const char *elem_type, const char *path, ncc_buffer_t *offs,
             ncc_buffer_t *asserts, int *count);
static void
gc_walk(ncc_xform_ctx_t *ctx, const char *elem_type, ncc_parse_tree_t *spec,
        const char *base, ncc_buffer_t *offs, ncc_buffer_t *asserts,
        int *count, variant_acc_t *vacc, bool *ok, int depth);

// One alternative's contribution to a variant descriptor: its selector
// typehash and the element-relative pointer word offsets that are live when the
// selector is active. `offs` is a CSV of offset expressions (as produced by
// emit_pointer / gc_walk). Alignment static_asserts go straight into the shared
// arrays buffer as they are discovered.
typedef struct {
    uint64_t      selector;
    ncc_buffer_t *offs;
    int           count;
} variant_arm_acc_t;

static int
cmp_variant_arm(const void *a, const void *b)
{
    uint64_t sa = ((const variant_arm_acc_t *)a)->selector;
    uint64_t sb = ((const variant_arm_acc_t *)b)->selector;
    return (sa < sb) ? -1 : (sa > sb) ? 1 : 0;
}

// Emit a variant descriptor for an n00b_variant_t field at `base`. `vunion` is
// the resolved value-union specifier. Each pointer-bearing alternative becomes
// an arm carrying the element-relative pointer offsets that are live when its
// selector is active: a single-pointer alternative has one offset (the value
// word); a by-value aggregate alternative has the pointer offsets of its fields
// (walked by gc_walk). On any alternative we cannot describe, sets *ok = false
// and warns — the whole type then falls back to conservative scan (never an
// under-scan).
static void
walk_variant(ncc_xform_ctx_t *ctx, const char *elem_type,
             ncc_parse_tree_t *vunion, const char *base, variant_acc_t *vacc,
             bool *ok, int depth)
{
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        vunion, "member_declaration_list");
    if (!members) {
        *ok = false;
        return;
    }

    ncc_layout_parse_tree_list_t alts = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &alts);

    char              *val_base    = path_join(base, "value");
    variant_arm_acc_t *arms        = ncc_alloc_array(variant_arm_acc_t, alts.len);
    size_t             narms       = 0;
    bool               unsupported = false;

    for (size_t i = 0; i < alts.len && !unsupported; i++) {
        uint64_t    h   = 0;
        alt_class_t cls = classify_variant_alt(ctx, alts.data[i], &h);

        if (cls == ALT_UNSUPPORTED) {
            unsupported = true;
            break;
        }
        if (cls == ALT_NONPTR) {
            continue; // no heap pointer in this alternative
        }

        ncc_parse_tree_t *aspecs = ncc_xform_find_child_nt(
            alts.data[i], "specifier_qualifier_list");
        char *field = aspecs ? ncc_layout_implicit_member_field_name(
                                   alts.data[i], aspecs)
                             : nullptr;
        if (!field) {
            unsupported = true;
            break;
        }
        char         *armbase = path_join(val_base, field);
        ncc_buffer_t *aoffs   = ncc_buffer_empty();
        int           acount  = 0;
        ncc_free(field);

        if (cls == ALT_PTR) {
            // The value word itself is the heap pointer.
            emit_pointer(elem_type, armbase, aoffs, vacc->arrays, &acount);
        }
        else { // ALT_AGGREGATE: walk the by-value struct's pointer fields.
            ncc_parse_tree_t *aspec = ncc_layout_aggregate_spec_from_specs(
                ctx, aspecs);
            // A nested variant inside an arm needs conditional nesting we don't
            // express yet; route it (and anything gc_walk can't fully describe)
            // to the conservative fallback. A throwaway vacc catches a nested
            // variant via its non-zero count.
            variant_acc_t inner = {
                .arrays = ncc_buffer_empty(),
                .inits  = ncc_buffer_empty(),
                .count  = 0,
                .rec    = vacc->rec,
            };
            bool aok = true;
            if (aspec) {
                gc_walk(ctx, elem_type, aspec, armbase, aoffs, vacc->arrays,
                        &acount, &inner, &aok, depth + 1);
            }
            bool inner_variant = inner.count != 0;
            ncc_free(inner.arrays->data);
            ncc_free(inner.arrays);
            ncc_free(inner.inits->data);
            ncc_free(inner.inits);
            if (!aspec || !aok || inner_variant) {
                ncc_free(armbase);
                ncc_free(aoffs->data);
                ncc_free(aoffs);
                unsupported = true;
                break;
            }
        }

        ncc_free(armbase);

        if (acount == 0) {
            // Alternative with no heap pointers: omit it from the arm table.
            ncc_free(aoffs->data);
            ncc_free(aoffs);
            continue;
        }

        arms[narms].selector = h;
        arms[narms].offs     = aoffs;
        arms[narms].count    = acount;
        narms++;
    }

    if (alts.data) {
        ncc_free(alts.data);
    }
    ncc_free(val_base);

    if (unsupported) {
        for (size_t i = 0; i < narms; i++) {
            ncc_free(arms[i].offs->data);
            ncc_free(arms[i].offs);
        }
        ncc_free(arms);
        fprintf(stderr,
                "ncc: warning: gc-typemap: type '%s' has an n00b_variant_t with "
                "an alternative that cannot be statically described for GC "
                "scanning; the type loses its precise pointer map and falls "
                "back to conservative scan (not precisely marshalable).\n",
                elem_type ? elem_type : "(anonymous)");
        *ok = false;
        return;
    }

    if (narms == 0) {
        // No pointer-bearing alternatives: precise, nothing to mark.
        ncc_free(arms);
        return;
    }

    // Sort arms by selector so the runtime can binary-search.
    qsort(arms, narms, sizeof(*arms), cmp_variant_arm);

    char *sel_path = path_join(base, "selector");
    char *sel_as   = offset_assert(elem_type, sel_path);
    char *sel_off  = offset_expr(elem_type, sel_path);
    int   k        = vacc->count;

    ncc_buffer_puts(vacc->arrays, sel_as);
    for (size_t a = 0; a < narms; a++) {
        ncc_buffer_printf(
            vacc->arrays,
            "static const uint64_t __ncc_gcvaroff_%zu_%d_%zu[]={%s};",
            vacc->rec, k, a, arms[a].offs->data ? arms[a].offs->data : "");
    }
    ncc_buffer_printf(
        vacc->arrays,
        "static const n00b_gc_variant_arm_t __ncc_gcvararms_%zu_%d[]={",
        vacc->rec, k);
    for (size_t a = 0; a < narms; a++) {
        ncc_buffer_printf(vacc->arrays,
                          "%s{.selector=%lluULL,.ptr_offset_count=%dULL,"
                          ".ptr_offsets=__ncc_gcvaroff_%zu_%d_%zu}",
                          a ? "," : "", (unsigned long long)arms[a].selector,
                          arms[a].count, vacc->rec, k, a);
    }
    ncc_buffer_puts(vacc->arrays, "};");

    if (k > 0) {
        ncc_buffer_puts(vacc->inits, ",");
    }
    ncc_buffer_printf(vacc->inits,
                      "{.selector_offset=%s,.arm_count=%zuULL,"
                      ".arms=__ncc_gcvararms_%zu_%d}",
                      sel_off, narms, vacc->rec, k);
    vacc->count++;

    ncc_free(sel_path);
    ncc_free(sel_as);
    ncc_free(sel_off);
    for (size_t a = 0; a < narms; a++) {
        ncc_free(arms[a].offs->data);
        ncc_free(arms[a].offs);
    }
    ncc_free(arms);
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
        variant_acc_t   *vacc,
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

// Does this member_declaration carry a *prefix* `[[n00b::noscan]]`
// attribute_specifier_sequence — i.e. one that appears before the type and
// therefore applies to every declarator in the declaration? (A C23 `[[...]]`
// may sit in the prefix position OR trail an individual declarator; the
// trailing case is detected per-declarator on the member_declarator node.)
static bool
member_decl_has_prefix_noscan(ncc_parse_tree_t *member)
{
    if (!member || ncc_tree_is_leaf(member)) {
        return false;
    }
    // The prefix attribute_specifier_sequence is a (group-unwrapped) direct
    // child of the member_declaration, per the grammar:
    //   <member_declaration> ::= <attribute_specifier_sequence>?
    //                            <specifier_qualifier_list>
    //                            <member_declarator_list>? ";"
    // ncc_xform_find_child_nt only descends into $$group_* wrappers, so it will
    // not reach a *trailing* attribute (which lives deeper, inside the
    // member_declarator_list subtree). That keeps a `[[n00b::noscan]]` on one
    // declarator from leaking onto a sibling in the same declaration.
    ncc_parse_tree_t *prefix_seq = ncc_xform_find_child_nt(
        member, "attribute_specifier_sequence");
    return prefix_seq
        && ncc_xform_subtree_carries_n00b_named_attr(prefix_seq, "noscan");
}

// Walk one member_declaration of a STRUCT.
static void
walk_struct_member(ncc_xform_ctx_t *ctx, const char *elem_type,
                   ncc_parse_tree_t *member, const char *base,
                   bool runtime_scan_shape, ncc_buffer_t *offs,
                   ncc_buffer_t *asserts, int *count, variant_acc_t *vacc,
                   bool *ok, int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return; // nothing typed here (e.g. a stray ';')
    }

    // A prefix `[[n00b::noscan]]` (before the type) applies to every declarator
    // in this member_declaration; a trailing one applies only to its own
    // declarator and is tested per-declarator below.
    bool prefix_noscan = member_decl_has_prefix_noscan(member);

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
            gc_walk(ctx, elem_type, nspec, nbase, offs, asserts, count, vacc,
                    ok, depth + 1);
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
                // No member_declarator_list wrapper here, so a trailing
                // `[[n00b::noscan]]` (if any) lives inside `member` itself;
                // either prefix or trailing form excludes this pointer.
                bool noscan = prefix_noscan
                           || ncc_xform_subtree_carries_n00b_named_attr(
                                  member, "noscan");
                if (!noscan
                    && !member_is_runtime_infra(runtime_scan_shape, field,
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
        // [[n00b::noscan]]: a prefix attribute on the whole declaration, or a
        // trailing attribute on this specific member_declarator, excludes the
        // field from the GC typemap (the GC must never scan/relocate it).
        if (prefix_noscan
            || ncc_xform_subtree_carries_n00b_named_attr(decls.data[i],
                                                         "noscan")) {
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
            // No '*' in the declarator. An INLINE struct/union/_generic_struct
            // is a by-value aggregate member — recurse into it. This must come
            // before the pointer-typedef test below: pointer_depth_for_specs
            // sees pointer MEMBERS in the inline body (e.g. an inline
            // n00b_list_t's `T *data`) and would wrongly classify the whole
            // by-value member as a pointer.
            ncc_parse_tree_t *nspec = ncc_layout_aggregate_spec_from_specs(
                ctx, member_specs);
            if (nspec) {
                gc_walk(ctx, elem_type, nspec, path, offs, asserts, count,
                        vacc, ok, depth + 1);
            }
            else if (ncc_layout_pointer_depth_for_specs(ctx, member_specs) > 0) {
                // No inline aggregate, but the SPECS still make this a pointer:
                // a pointer typedef or an _Atomic(T *) wrapper.
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
            // else: scalar by-value member -> no pointer words.
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
        int *count, variant_acc_t *vacc, bool *ok, int depth)
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

    // An n00b_variant_t is precisely scannable via its selector discriminant
    // even though its value union mixes pointer and scalar alternatives. Handle
    // it before the generic union/struct logic so the conservative union path
    // never sees (and warns about) the variant's value union.
    ncc_parse_tree_t *vunion = variant_value_union(ctx, spec);
    if (vunion) {
        walk_variant(ctx, elem_type, vunion, base, vacc, ok, depth);
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
                           runtime_scan_shape, offs, asserts, count, vacc, ok,
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

    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (data && !data->gc_typemaps) {
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

static char *
derive_trmap_attr(const char *stobj_attr)
{
    return derive_gc_section_attr(stobj_attr, "n00b_trmap");
}

static char *
derive_tridx_attr(const char *stobj_attr)
{
    return derive_gc_section_attr(stobj_attr, "n00b_tridx");
}

// ---------------------------------------------------------------------------
// [[n00b::transient]] walk (WP-001). Collects the fields the programmer marked
// as raw BYTE offset + BYTE size pairs. Unlike gc_walk this is NOT coupled to
// pointer conservatism: it records exactly the marked fields, of any type and
// any width — a transient field may be a sub-word scalar such as an `int` fd —
// so it assumes NO word alignment (no /sizeof(void*), no alignment assert).
// Marshal zeroes these byte ranges on the content-hash / store path.
// ---------------------------------------------------------------------------

static void
tr_emit_field(const char *elem_type, const char *path,
              ncc_buffer_t *offs, ncc_buffer_t *sizes, int *count)
{
    char *oe = ncc_layout_format_cstr("__builtin_offsetof(%s,%s)", elem_type,
                                      path);
    char *se = ncc_layout_format_cstr("sizeof(((%s *)0)->%s)", elem_type, path);
    if (*count > 0) {
        ncc_buffer_puts(offs, ",");
        ncc_buffer_puts(sizes, ",");
    }
    ncc_buffer_puts(offs, oe);
    ncc_buffer_puts(sizes, se);
    (*count)++;
    ncc_free(oe);
    ncc_free(se);
}

// A *prefix* `[[n00b::transient]]` (before the type) applies to every
// declarator in the member_declaration; a *trailing* one is per-declarator.
// Mirrors member_decl_has_prefix_noscan.
static bool
member_decl_has_prefix_transient(ncc_parse_tree_t *member)
{
    if (!member || ncc_tree_is_leaf(member)) {
        return false;
    }
    ncc_parse_tree_t *prefix_seq = ncc_xform_find_child_nt(
        member, "attribute_specifier_sequence");
    return prefix_seq
        && ncc_xform_subtree_carries_n00b_named_attr(prefix_seq, "transient");
}

static void
tr_walk(ncc_xform_ctx_t *ctx, const char *elem_type, ncc_parse_tree_t *spec,
        const char *base, ncc_buffer_t *offs, ncc_buffer_t *sizes, int *count,
        int depth);

static void
tr_walk_member(ncc_xform_ctx_t *ctx, const char *elem_type,
               ncc_parse_tree_t *member, const char *base, ncc_buffer_t *offs,
               ncc_buffer_t *sizes, int *count, int depth)
{
    ncc_parse_tree_t *member_specs = ncc_xform_find_child_nt(
        member, "specifier_qualifier_list");
    if (!member_specs) {
        return;
    }
    bool prefix_transient = member_decl_has_prefix_transient(member);

    ncc_parse_tree_t *member_list = ncc_xform_find_child_nt(
        member, "member_declarator_list");

    if (!member_list) {
        // Anonymous by-value aggregate: recurse with the base unchanged (its
        // members are accessed by their own name in offsetof). A prefix mark on
        // a whole anonymous aggregate is not a v1 case.
        ncc_parse_tree_t *nspec = ncc_layout_aggregate_spec_from_specs(
            ctx, member_specs);
        if (nspec) {
            tr_walk(ctx, elem_type, nspec, base, offs, sizes, count, depth + 1);
            return;
        }
        // Single declarator with no list wrapper (e.g. a typedef-pointer
        // member): mark it if prefix- or (member-local) trailing-transient.
        if (prefix_transient
            || ncc_xform_subtree_carries_n00b_named_attr(member, "transient")) {
            char *field = ncc_layout_implicit_member_field_name(member,
                                                                member_specs);
            if (field) {
                char *path = path_join(base, field);
                tr_emit_field(elem_type, path, offs, sizes, count);
                ncc_free(path);
                ncc_free(field);
            }
        }
        return;
    }

    ncc_layout_parse_tree_list_t decls = {0};
    ncc_layout_collect_nt_children(member_list, "member_declarator", &decls);
    for (size_t i = 0; i < decls.len; i++) {
        ncc_parse_tree_t *declarator = ncc_xform_find_child_nt(decls.data[i],
                                                              "declarator");
        if (!declarator) {
            continue;
        }
        char *name = ncc_layout_declarator_name(declarator);
        if (!name) {
            continue;
        }
        bool marked = prefix_transient
                   || ncc_xform_subtree_carries_n00b_named_attr(decls.data[i],
                                                                "transient");
        if (marked) {
            // The whole field is transient — zero its bytes. Works for a
            // scalar, a pointer, an array, or an entire by-value sub-aggregate.
            char *path = path_join(base, name);
            tr_emit_field(elem_type, path, offs, sizes, count);
            ncc_free(path);
        }
        else {
            // Unmarked: descend into a NAMED by-value aggregate member to find
            // nested transient leaves (offsetof path: base.name).
            bool is_ptr = ncc_layout_pointer_depth_for_declarator(
                              ctx, member_specs, declarator)
                       > 0;
            bool is_array = ncc_layout_first_descendant_nt(declarator,
                                                          "array_declarator")
                         != nullptr;
            if (!is_ptr && !is_array) {
                ncc_parse_tree_t *nspec = ncc_layout_aggregate_spec_from_specs(
                    ctx, member_specs);
                if (nspec) {
                    char *path = path_join(base, name);
                    tr_walk(ctx, elem_type, nspec, path, offs, sizes, count,
                            depth + 1);
                    ncc_free(path);
                }
            }
        }
        ncc_free(name);
    }
    if (decls.data) {
        ncc_free(decls.data);
    }
}

static void
tr_walk(ncc_xform_ctx_t *ctx, const char *elem_type, ncc_parse_tree_t *spec,
        const char *base, ncc_buffer_t *offs, ncc_buffer_t *sizes, int *count,
        int depth)
{
    if (depth > 64 || *count > NCC_GCMAP_MAX_OFFSETS) {
        return;
    }
    spec = ncc_layout_resolve_aggregate_specifier(ctx, spec);
    if (!spec) {
        return;
    }
    ncc_parse_tree_t *members = ncc_xform_find_child_nt(
        spec, "member_declaration_list");
    if (!members) {
        return;
    }
    ncc_layout_parse_tree_list_t mlist = {0};
    ncc_layout_collect_nt_children(members, "member_declaration", &mlist);
    for (size_t i = 0; i < mlist.len; i++) {
        tr_walk_member(ctx, elem_type, mlist.data[i], base, offs, sizes, count,
                       depth);
    }
    if (mlist.data) {
        ncc_free(mlist.data);
    }
}

char *
ncc_gc_typemap_emit(ncc_xform_ctx_t *ctx)
{
    ncc_buffer_t *out      = ncc_buffer_empty();
    size_t        n_emitted = 0;

    ncc_xform_data_t *data = ncc_xform_get_data(ctx);
    if (data && !data->gc_typemaps) {
        return ncc_buffer_take(out);
    }

    const char   *stobj_attr = data ? data->static_object_entry_attr : nullptr;
    char         *gcmap_attr = derive_gcmap_attr(stobj_attr);
    char         *gcidx_attr = derive_gcidx_attr(stobj_attr);
    char         *trmap_attr = derive_trmap_attr(stobj_attr);
    char         *tridx_attr = derive_tridx_attr(stobj_attr);

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
        ncc_parse_tree_t *spec = nullptr;
        if (!scalar_no_ptr) {
            // Aggregate resolution now flows through the type model: the shared
            // lookup falls back to the symbol table, so _generic_struct typedefs
            // (the n00b_variant_t / generic-container shape) resolve here too.
            ncc_layout_aggregate_type_info_t *info =
                ncc_layout_aggregate_info_from_type_name(ctx,
                                                         records[i].elem_type);
            if (!info || !info->specifier
                || !aggregate_type_is_file_visible(info) || info->is_atomic) {
                continue;
            }
            spec = info->specifier;
        }

        ncc_buffer_t *offs    = ncc_buffer_empty();
        ncc_buffer_t *asserts = ncc_buffer_empty();
        int           count   = 0;
        bool          ok      = true;
        variant_acc_t vacc    = {
                  .arrays = ncc_buffer_empty(),
                  .inits  = ncc_buffer_empty(),
                  .count  = 0,
                  .rec    = i,
        };

        // [[n00b::transient]] (WP-001): collected independently of the GC
        // pointer walk — a type can carry transient fields even when its
        // precise pointer map bails (ok=false). Byte offsets + sizes.
        ncc_buffer_t *tr_offs  = ncc_buffer_empty();
        ncc_buffer_t *tr_sizes = ncc_buffer_empty();
        int           tr_count = 0;

        if (!scalar_no_ptr) {
            gc_walk(ctx, records[i].elem_type, spec, "", offs, asserts,
                    &count, &vacc, &ok, 0);
            tr_walk(ctx, records[i].elem_type, spec, "", tr_offs, tr_sizes,
                    &tr_count, 0);
        }

        if (ok) {
            char *asserts_str = ncc_buffer_take(asserts);
            char *var_arrays  = ncc_buffer_take(vacc.arrays);
            char *var_inits   = ncc_buffer_take(vacc.inits);

            // Asserts + per-variant ptr-hash arrays first (file scope), then the
            // descriptor + section entry.
            ncc_buffer_printf(out, "%s", asserts_str);
            ncc_buffer_printf(out, "%s", var_arrays);
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
            if (vacc.count > 0) {
                ncc_buffer_printf(
                    out,
                    "static const n00b_gc_variant_field_t "
                    "__ncc_gcmap_var_%zu[]={%s};",
                    i, var_inits);
            }
            char *offsets_expr = count > 0
                               ? ncc_layout_format_cstr("__ncc_gcmap_off_%zu", i)
                               : ncc_layout_copy_cstr("nullptr");
            char *variants_expr = vacc.count > 0
                                ? ncc_layout_format_cstr("__ncc_gcmap_var_%zu", i)
                                : ncc_layout_copy_cstr("nullptr");
            ncc_buffer_printf(
                out,
                "static const n00b_gc_struct_layout_t __ncc_gcmap_lay_%zu="
                "{.stride=(sizeof(%s)/sizeof(void*)),.count=1,.offset_count=%d,"
                ".offsets=%s,.variant_count=%dULL,.variants=%s};",
                i, records[i].elem_type, count, offsets_expr, vacc.count,
                variants_expr);
            ncc_free(variants_expr);
            ncc_free(var_arrays);
            ncc_free(var_inits);
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
            ncc_free(ncc_buffer_take(vacc.arrays));
            ncc_free(ncc_buffer_take(vacc.inits));
        }

        // Transient table for this type — emitted whether or not the GC
        // pointer map succeeded (ok). The trmap ENTRY carries the real
        // type_hash, so the marshal reader can scan n00b_trmap directly; the
        // tridx placeholder is a post-link-fillable acceleration index that
        // mirrors n00b_gcidx.
        if (tr_count > 0) {
            char *tr_off_csv = ncc_buffer_take(tr_offs);
            char *tr_sz_csv  = ncc_buffer_take(tr_sizes);
            ncc_buffer_printf(
                out,
                "static const uint64_t __ncc_trmap_off_%zu[]={%s};",
                i, tr_off_csv);
            ncc_buffer_printf(
                out,
                "static const uint64_t __ncc_trmap_sz_%zu[]={%s};",
                i, tr_sz_csv);
            ncc_buffer_printf(
                out,
                "static const n00b_transient_layout_t __ncc_trmap_lay_%zu="
                "{.field_count=%dULL,.byte_offsets=__ncc_trmap_off_%zu,"
                ".byte_sizes=__ncc_trmap_sz_%zu};",
                i, tr_count, i, i);
            ncc_buffer_printf(
                out,
                "%s static const n00b_transient_map_entry_t "
                "__ncc_trmap_ent_%zu={.type_hash=%lluULL,.layout="
                "&__ncc_trmap_lay_%zu};",
                trmap_attr, i, (unsigned long long)records[i].hash, i);
            ncc_buffer_printf(
                out,
                "%s static const n00b_transient_map_index_entry_t "
                "__ncc_tridx_ent_%zu={.type_hash=0ULL,.entry_index=%zuULL};",
                tridx_attr, i, i);
            ncc_free(tr_off_csv);
            ncc_free(tr_sz_csv);
            n_emitted++;
        }
        else {
            ncc_free(ncc_buffer_take(tr_offs));
            ncc_free(ncc_buffer_take(tr_sizes));
        }
    }

    ncc_free(gcmap_attr);
    ncc_free(gcidx_attr);
    ncc_free(trmap_attr);
    ncc_free(tridx_attr);

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
