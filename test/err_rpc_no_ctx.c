// err_rpc_no_ctx — @rpc handler must take a trailing
// `n00b_rpc_ctx_t *` parameter; missing it is rejected.

typedef int n00b_err_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;

_generic_struct typeid("result", Reply_t *) {
    bool is_ok; union { Reply_t *ok; n00b_err_t err; };
};

_generic_struct typeid("result", Reply_t *)
hello(Req_t *req) @rpc("a.b.C/D")
{
    (void)req;
    return (_generic_struct typeid("result", Reply_t *)){0};
}

int main(void) { return 0; }
