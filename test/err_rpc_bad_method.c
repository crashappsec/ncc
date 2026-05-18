// err_rpc_bad_method — the @rpc string must match the form
// "<package>.<Service>/<Method>"; a single bare word is rejected.

typedef int n00b_err_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;
typedef struct { int p; } n00b_rpc_ctx_t;

_generic_struct typeid("result", Reply_t *) {
    bool is_ok; union { Reply_t *ok; n00b_err_t err; };
};

_generic_struct typeid("result", Reply_t *)
hello(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("not_a_valid_method")
{
    (void)req;
    (void)ctx;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

int main(void) { return 0; }
