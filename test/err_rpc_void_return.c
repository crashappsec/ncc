// err_rpc_void_return — @rpc handler must return n00b_result_t(T *);
// a void return is rejected by the shape-detection diagnostic.

typedef struct { int x; } Req_t;
typedef struct { int p; } n00b_rpc_ctx_t;

void
hello(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("a.b.C/D")
{
    (void)req;
    (void)ctx;
}

int main(void) { return 0; }
