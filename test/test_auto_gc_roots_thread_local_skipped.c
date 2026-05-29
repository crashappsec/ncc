// test_auto_gc_roots_thread_local_skipped.c
//
// WP-003 / D-036 (F-2) skip-rule regression test. Decls carrying
// `thread_local` (C23) or `_Thread_local` (C11) storage class must
// NOT be auto-registered: their address is not a compile-time
// constant (each thread gets its own copy), so the static-table
// emit path cannot embed `&var`. The transform recognizes the
// storage class and skips, mirroring the `extern` skip.
//
// The meson wiring asserts via `preprocess_not_contains` that the
// emitted source contains no `& tls_singleton_*` reference (the
// shape ncc emits for a registered entry: `(void *) & <name>`).
// The flag is on, so the test source also includes a *registering*
// decl so the file is not skipped entirely by the zero-qualifying-
// decls rule. The presence of `plain_root` means a table is
// produced; the absence of the thread_local entries is what we are
// pinning.

typedef struct n00b_string_t n00b_string_t;

thread_local n00b_string_t *tls_singleton_c23  = ((void *)0);
_Thread_local n00b_string_t *tls_singleton_c11 = ((void *)0);
static n00b_string_t *plain_root = ((void *)0);

int
main(void)
{
    return (plain_root == ((void *)0)
            && tls_singleton_c23 == ((void *)0)
            && tls_singleton_c11 == ((void *)0)) ? 0 : 1;
}
