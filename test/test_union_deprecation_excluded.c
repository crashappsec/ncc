// test_union_deprecation_excluded.c -- unions the deprecation pass must NOT
// flag: an n00b_variant_t value-union and a `[[n00b::raw_union]]`-annotated
// union. Neither should produce a "traditional C union" diagnostic.
// Used by ncc_union_deprecation_excluded.

#include <stdint.h>

typedef struct n00b_string_t {
    uint64_t length;
    char    *data;
} n00b_string_t;

// A real n00b_variant_t (the _generic_struct form the macro lowers to),
// referenced via typehash below so the type model resolves it — exactly how
// the GC-typemap detector sees it. Its value-union must be excluded.
typedef _generic_struct typeid("n00b_variant", n00b_string_t *, uint64_t) {
    uint64_t selector;
    union {
        n00b_string_t *field_0;
        uint64_t       field_1;
    } value;
} my_variant_t;

// The escape hatch: a deliberately-raw union must be excluded.
typedef struct punned_t {
    int32_t tag;
    union [[n00b::raw_union]] {
        float    f;
        uint32_t bits;
    } pun;
} punned_t;

static uint64_t h = typehash(my_variant_t *);

int
main(void)
{
    return (int)(h == 0);
}
