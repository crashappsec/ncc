// test_nodiscard_warn.c — a discarded result-protocol return is flagged.
// `mk` returns a result-shaped aggregate (is_ok / ok / err); calling it as a
// bare statement drops the result, so the must-check pass must warn.

#define RES(T) _generic_struct typeid("result", T) { int is_ok; T ok; int err; }

static RES(int)
mk(int v)
{
    return (RES(int)){.is_ok = 1, .ok = v, .err = 0};
}

int
main(void)
{
    mk(1); // WARN: result of 'mk()' is ignored
    return 0;
}
