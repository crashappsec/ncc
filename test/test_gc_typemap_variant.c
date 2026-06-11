// test_gc_typemap_variant.c -- discriminated-union (n00b_variant_t) GC maps.
//
// Meson runs this source through `ncc -E` and asserts on the transformed
// output. An n00b_variant_t is shaped `{ uint64_t selector; union {
// T field_...; ... } value; }`, where `selector` holds typehash(T) of the
// active alternative. ncc must describe it precisely (a variant descriptor +
// the selector/value offsets + a pointer-alternative typehash table) instead
// of warning and falling back to a conservative scan, both when the variant is
// the whole object and when it is a field of a larger struct.
//
// The detector is SHAPE-based (selector:uint64_t + value:union whose members
// are all named field_*), so the fixture spells the post-expansion shape with
// plain structs rather than the n00b_variant_t macro; that is exactly what the
// macro's `_generic_struct` resolves to by the time the GC-map walker runs.

#include <stdint.h>

typedef struct n00b_string_t {
    uint64_t length;
    char    *data;
} n00b_string_t;

typedef struct vmap_payload_t {
    uint64_t value;
} vmap_payload_t;

// A variant with one pointer alternative and one scalar alternative: the value
// word is a heap pointer iff the selector names the pointer alternative.
typedef struct vmap_variant_t {
    uint64_t selector;
    union {
        n00b_string_t *field_0;
        uint64_t       field_1;
    } value;
} vmap_variant_t;

// The same variant embedded as a field of a larger struct, alongside an
// ordinary (unconditional) pointer field.
typedef struct vmap_holder_t {
    vmap_payload_t *head;
    vmap_variant_t  choice;
} vmap_holder_t;

// An all-scalar variant carries no heap pointer: it stays precise (no variant
// descriptor needed) and must not warn.
typedef struct vmap_scalar_variant_t {
    uint64_t selector;
    union {
        uint64_t field_0;
        int64_t  field_1;
    } value;
} vmap_scalar_variant_t;

// The real n00b_variant_t shape: a _generic_struct typedef. The legacy
// aggregate table cannot resolve this (its alias is recorded before
// generic-struct lowering), so it used to be skipped silently. The type model
// resolves it, so it now gets a precise variant descriptor too.
typedef _generic_struct typeid("n00b_variant", n00b_string_t *, uint64_t) {
    uint64_t selector;
    union {
        n00b_string_t *field_0;
        uint64_t       field_1;
    } value;
} vmap_generic_variant_t;

static uint64_t h_variant = typehash(vmap_variant_t *);
static uint64_t h_holder  = typehash(vmap_holder_t *);
static uint64_t h_scalar  = typehash(vmap_scalar_variant_t *);
static uint64_t h_generic = typehash(vmap_generic_variant_t *);

int
main(void)
{
    return (int)((h_variant ^ h_holder ^ h_scalar ^ h_generic) == 0);
}
