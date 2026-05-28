// test_auto_gc_roots_const_char_star_skipped.c
//
// Phase 2 skip-rule regression test (spec § 9
// "_const_char_star_skipped", spec § 2.2 row 3). The
// initializer-shape skip rule: with `--ncc-auto-gc-roots` on, a
// `const char *s = "literal";` TU-scope definition must NOT
// register (the initializer is an `.rodata` string literal, not
// managed memory). A `const char *t = some_buffer;` (non-literal
// initializer) DOES register — the skip is initializer-aware ONLY
// for the const-char-pointer case.
//
// The meson wiring asserts:
//   - `preprocess_not_contains` "& literal_str"  (the literal-init
//     case is skipped).
//   - `preprocess_contains` "& buffer_ref"       (the non-literal
//     case registers).
//
// Both assertions are needed: presence-of-the-non-literal entry
// confirms the transform is firing on this source; absence-of-the-
// literal entry confirms the per-decl skip rule.

static const char *literal_str    = "this is a literal";
static char        some_buffer[8] = {0};
static const char *buffer_ref     = some_buffer;

int
main(void)
{
    return (literal_str != ((void *)0) && buffer_ref != ((void *)0)) ? 0 : 1;
}
