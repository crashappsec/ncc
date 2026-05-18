// err_rpc_with_kargs — `@rpc` and `_kargs` on the same function are
// rejected at parse time (spec §4); the grammar accepts the
// combination so the xform can produce a precise diagnostic
// instead of a generic parse failure.

typedef int n00b_err_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;
typedef struct { int p; } n00b_rpc_ctx_t;

_generic_struct typeid("result", Reply_t *) {
    bool is_ok; union { Reply_t *ok; n00b_err_t err; };
};

_generic_struct typeid("result", Reply_t *)
hello(Req_t *req, n00b_rpc_ctx_t *ctx)
    _kargs { int extra = 0; }
    @rpc("a.b.C/D")
{
    (void)req;
    (void)ctx;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

int main(void) { return 0; }
