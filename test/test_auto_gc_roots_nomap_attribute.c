// test_auto_gc_roots_nomap_attribute.c
//
// Phase 5 regression test (spec § 9 "_nomap_attribute"). A TU-scope
// pointer-scalar carrying `[[n00b::nomap]]` is silently skipped:
// no table entry emitted, no diagnostic. A sibling decl without the
// attribute IS registered, so we can pin both the negative and
// positive halves on the same source.
//
// Covers both spec § 3.1 attribute positions:
//   * prefix    — `[[n00b::nomap]] static T *legacy_singleton;`
//   * trailing  — `T *trailing_nomap [[n00b::nomap]];`

typedef struct n00b_string_t n00b_string_t;
typedef struct n00b_buffer_t n00b_buffer_t;

[[n00b::nomap]] static n00b_string_t *legacy_singleton;
static n00b_buffer_t                 *trailing_nomap [[n00b::nomap]];
static n00b_string_t                 *registered_root;

int
main(void)
{
    return (legacy_singleton == ((void *)0)
            && trailing_nomap == ((void *)0)
            && registered_root == ((void *)0))
               ? 0
               : 1;
}
