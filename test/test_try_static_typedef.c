// test_try_static_typedef.c — regression for return_type_text in the `_try`
// lowering. `_try` builds a wrapper whose return type is the enclosing
// function's return type, so that type must be extracted correctly from the
// declaration_specifiers. The hard case is a typedef-name return type behind
// storage-class / function specifiers (`static inline res_t`): the type must be
// `res_t`, with `static`/`inline` dropped — exercising the parse-tree-based
// specifier classification (not textual keyword stripping).

typedef struct {
    int is_ok;
    int ok;
    int err;
} res_t;

static res_t
ok_(int v)
{
    return (res_t){.is_ok = 1, .ok = v, .err = 0};
}

static res_t
bad(int e)
{
    return (res_t){.is_ok = 0, .ok = 0, .err = e};
}

// Return type is `static inline res_t` — the wrapper must be `res_t`, not
// `int`/`inline`/etc.
static inline res_t
unwrap_ok(void)
{
    int v = _try ok_(21);
    return ok_(v * 2);
}

static inline res_t
propagate(void)
{
    int v = _try bad(7); // returns the error result; the next line is skipped
    return ok_(v);
}

int
main(void)
{
    res_t a = unwrap_ok();
    if (!a.is_ok || a.ok != 42) {
        return 1;
    }
    res_t b = propagate();
    if (b.is_ok || b.err != 7) {
        return 2;
    }
    return 0;
}
