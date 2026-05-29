// err_auto_gc_roots_incomplete_struct.c
//
// Phase 5 warn-and-skip regression test (D-009). A TU-scope decl
// whose struct type is incomplete (forward-declared only, with no
// member list available at the decl site) must:
//   1. emit a build warning naming the decl and the unresolved type
//      and suggesting `[[n00b::nomap]]` or a full definition, and
//   2. NOT produce a registration entry.
//   3. ncc exits 0 (the build does not fail).
//
// Meson wiring uses the `preprocess_stderr_contains` runner mode to
// assert the warning text and exit-0 simultaneously.

struct opaque;

static struct opaque *forward_ptr;
static struct opaque  forward_obj;

int
main(void)
{
    return forward_ptr == ((void *)0) ? 0 : 1;
}
