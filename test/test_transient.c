// test_transient.c -- WP-001 [[n00b::transient]] field-attribute fixture.
//
// Meson runs this through `ncc -E` and asserts on the transformed output:
//   * a transient field is emitted into a BYTE-granular n00b_trmap table as a
//     raw __builtin_offsetof + sizeof pair -- NO /sizeof(void*) and no
//     word-alignment static_assert (a transient field may be a sub-word
//     scalar such as an `int` fd);
//   * non-transient fields stay OUT of the transient table;
//   * GC scanning is unaffected -- a transient *pointer* field still appears
//     in the n00b_gcmap pointer map (transient != noscan);
//   * the ncc-only attribute is stripped before the C is emitted.

#include <stdint.h>

typedef struct trans_payload_t {
    uint64_t value;
} trans_payload_t;

// Trailing transient on a sub-word scalar; the non-transient pointer sibling
// must remain in the GC pointer map and stay OUT of the transient table.
typedef struct trans_trailing_t {
    trans_payload_t *keep_ptr;
    int              fd [[n00b::transient]];
} trans_trailing_t;

// Prefix transient applies to the whole declaration: a transient pointer that
// must appear in the transient table AND still be GC-scanned in n00b_gcmap.
typedef struct trans_prefix_t {
    trans_payload_t          *keep_ptr2;
    [[n00b::transient]] trans_payload_t *handle;
} trans_prefix_t;

// No transient fields: emits NO transient table; a plain scalar appears in
// neither the GC map nor the transient table.
typedef struct trans_none_t {
    trans_payload_t *p;
    int              plain;
} trans_none_t;

static uint64_t h_trail = typehash(trans_trailing_t *);
static uint64_t h_pref  = typehash(trans_prefix_t *);
static uint64_t h_none  = typehash(trans_none_t *);

int
main(void)
{
    return (int)((h_trail ^ h_pref ^ h_none) == 0);
}
