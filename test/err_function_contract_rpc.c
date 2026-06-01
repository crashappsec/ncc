// err_function_contract_rpc - v1 rejects @rpc plus function contracts with
// a specific diagnostic instead of a generic parse failure.

typedef int n00b_err_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;
typedef struct { int p; } n00b_rpc_ctx_t;

_generic_struct typeid("result", Reply_t *) {
    bool is_ok;
    union {
        Reply_t *ok;
        n00b_err_t err;
    };
};

_generic_struct typeid("result", Reply_t *)
hello(Req_t *req, n00b_rpc_ctx_t *ctx)
    @rpc("a.b.C/D")
    requires { req != nullptr; }
{
    (void)req;
    (void)ctx;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

int
main(void)
{
    return 0;
}
