// test_defer.c — behavioral test for `_defer` (N3199 semantics).
//
// Verifies: deferred blocks run at scope exit in LIFO order; on early return;
// on loop continue/break; that the return value is computed BEFORE deferred
// blocks run; and that defers see current variable values.

int idx = 0;
int log_[64];

static void
rec(int v)
{
    log_[idx++] = v;
}

static int
basic(void)
{
    rec(1);
    _defer { rec(99); }
    rec(2);
    return 0;
} // expect: 1, 2, 99

static int
lifo(void)
{
    _defer { rec(10); }
    _defer { rec(20); }
    return 0;
} // expect: 20, 10

static int
early(int x)
{
    _defer { rec(5); }
    if (x) {
        return 1;
    }
    rec(6);
    return 0;
}

static int
loopy(void)
{
    for (int i = 0; i < 3; i++) {
        _defer { rec(100 + i); } // runs at end of each iteration's body
        if (i == 1) {
            continue;
        }
        if (i == 2) {
            break;
        }
        rec(i);
    }
    return 0;
} // expect: 0, 100, 101, 102

// The return value is computed before deferred blocks run, so the mutation of
// `v` in the defer does not affect the returned value.
static int
retval(void)
{
    int v = 7;
    _defer { v = 999; }
    return v;
} // returns 7

#define CHECK(cond)        \
    do {                   \
        if (!(cond)) {     \
            return __LINE__; \
        }                  \
    } while (0)

int
main(void)
{
    idx = 0;
    basic();
    CHECK(idx == 3 && log_[0] == 1 && log_[1] == 2 && log_[2] == 99);

    idx = 0;
    lifo();
    CHECK(idx == 2 && log_[0] == 20 && log_[1] == 10);

    idx = 0;
    early(1);
    CHECK(idx == 1 && log_[0] == 5);

    idx = 0;
    early(0);
    CHECK(idx == 2 && log_[0] == 6 && log_[1] == 5);

    idx = 0;
    loopy();
    CHECK(idx == 4 && log_[0] == 0 && log_[1] == 100 && log_[2] == 101
          && log_[3] == 102);

    CHECK(retval() == 7);

    return 0;
}
