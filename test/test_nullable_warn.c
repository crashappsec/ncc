// test_nullable_warn.c — stage 2: an unchecked dereference of a `?` (nullable)
// pointer is reported; a null-checked one is not. Checked via stderr (the
// warning must appear) and stdout (the `?` qualifier must be stripped).

int
deref_unchecked(?int *p)
{
    return *p; // WARN: possible null dereference of nullable 'p'
}

int
deref_guarded(?int *p)
{
    if (!p) {
        return 0;
    }
    return *p; // OK: p proven non-null by the early return
}
