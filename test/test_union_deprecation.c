// test_union_deprecation.c -- a traditional (non-variant) C union. The
// union-deprecation pass warns on it (and, with --ncc-error-on-union, errors).
// Used by ncc_union_deprecation_{warn,allow,error}.

#include <stdint.h>

typedef struct n00b_string_t {
    uint64_t length;
    char    *data;
} n00b_string_t;

// A hand-rolled tagged union: a discriminant plus a bare `union`. This is
// exactly the shape the deprecation steers toward n00b_variant_t.
typedef struct trad_tagged_t {
    int32_t kind;
    union {
        int64_t        as_int;
        n00b_string_t *as_str;
    } payload;
} trad_tagged_t;

int
main(void)
{
    trad_tagged_t t = {.kind = 0};
    return (int)t.kind;
}
