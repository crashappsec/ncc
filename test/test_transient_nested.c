// test_transient_nested.c -- [[n00b::transient]] on a field of a NESTED
// by-value aggregate, including one reached through an _Atomic(struct)
// typedef (the n00b_pinref_t shape).
//
// Regression: the transient walk dropped the containing member's name when
// the parser surfaced a typedef-named member without a member_declarator_list,
// emitting __builtin_offsetof(outer_t,pins) -- "no member named 'pins'" -- and
// never handled the _Atomic boundary, through which offsetof cannot descend.
// This fixture is compiled and run, so the emitted trmap must be valid C.

#include <stdint.h>
#include <stddef.h>

typedef struct tn_payload_t {
    uint64_t value;
} tn_payload_t;

typedef struct {
    void    *raw;
    uint32_t pins [[n00b::transient]];
    uint32_t lock : 1;
} tn_raw_pinref_t;

typedef _Atomic tn_raw_pinref_t tn_pinref_t;

// Typedef-named _Atomic aggregate member (the n00b dict store_pin shape).
typedef struct tn_atomic_outer_t {
    tn_payload_t *head;
    uint64_t      pad;
    tn_pinref_t   store_pin;
} tn_atomic_outer_t;

// Typedef-named NON-atomic aggregate member.
typedef struct tn_plain_outer_t {
    uint64_t        pad;
    tn_raw_pinref_t ref;
} tn_plain_outer_t;

// Inline _Atomic(struct { ... }) member (the n00b_pinref(T) shape a typed
// dict uses). The `*` of `raw` is inside the struct body: the member is an
// atomic struct, not an atomic pointer, so its transient leaves are found.
typedef struct tn_inline_outer_t {
    tn_payload_t *head;
    _Atomic(struct {
        tn_payload_t *raw;
        uint32_t      pins [[n00b::transient]];
        uint32_t      lock [[n00b::transient]];
    }) store_pin;
} tn_inline_outer_t;

// `_Atomic T` qualifier on a typedef-named aggregate member.
typedef struct tn_qual_outer_t {
    uint64_t                pad;
    _Atomic tn_raw_pinref_t ref;
} tn_qual_outer_t;

static uint64_t h_atomic = typehash(tn_atomic_outer_t *);
static uint64_t h_plain  = typehash(tn_plain_outer_t *);
static uint64_t h_inline = typehash(tn_inline_outer_t *);
static uint64_t h_qual   = typehash(tn_qual_outer_t *);

int
main(void)
{
    return (int)((h_atomic ^ h_plain) == 0 || (h_inline ^ h_qual) == 0);
}
