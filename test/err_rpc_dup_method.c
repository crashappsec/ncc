// err_rpc_dup_method — two `@rpc(...)` handlers in the same
// translation unit binding the same method string conflict;
// the xform detects + diagnoses the second occurrence.

typedef int n00b_err_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;
typedef struct { int p; } n00b_rpc_ctx_t;

_generic_struct typeid("result", Reply_t *) {
    bool is_ok; union { Reply_t *ok; n00b_err_t err; };
};

_generic_struct typeid("result", Reply_t *)
first(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("a.b.C/D")
{
    (void)req; (void)ctx;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

_generic_struct typeid("result", Reply_t *)
second(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("a.b.C/D")
{
    (void)req; (void)ctx;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

int main(void) { return 0; }
