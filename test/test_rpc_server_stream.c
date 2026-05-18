// test_rpc_server_stream — Phase E server-stream end-to-end.
//
// Handler returns a typed stream; dispatcher is registered via
// `n00b_rpc_register_server_stream`; client stub round-trips
// through `n00b_rpc_call_server_stream`. The runtime treats typed
// and buffer streams as structural aliases, so the
// dispatcher / stub cast at the boundary.

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int n00b_err_t;
typedef struct { int p; } n00b_buffer_t;
typedef struct { int p; } n00b_rpc_ctx_t;
typedef struct { int p; } n00b_rpc_channel_t;
typedef struct { int x; } Req_t;
typedef struct { int y; } StreamItem;

// Stream-of-T bodies. The runtime's stream is a forward-declared
// struct; here we provide minimal bodies under the same typeid tag
// the ncc-emitted code references.
_generic_struct typeid("rpc_stream", n00b_buffer_t *) { void *_opaque; };
_generic_struct typeid("rpc_stream", StreamItem)      { void *_opaque; };

// Result bodies.
_generic_struct typeid("result", Req_t *) {
    bool is_ok; union { Req_t *ok; n00b_err_t err; };
};
_generic_struct typeid("result", _generic_struct typeid("rpc_stream", StreamItem) *) {
    bool is_ok;
    union {
        _generic_struct typeid("rpc_stream", StreamItem) *ok;
        n00b_err_t err;
    };
};
_generic_struct typeid("result", _generic_struct typeid("rpc_stream", n00b_buffer_t *) *) {
    bool is_ok;
    union {
        _generic_struct typeid("rpc_stream", n00b_buffer_t *) *ok;
        n00b_err_t err;
    };
};

typedef _generic_struct typeid("result", _generic_struct typeid("rpc_stream", n00b_buffer_t *) *)
    (*ss_dispatch_fn_t)(n00b_buffer_t *, n00b_rpc_ctx_t *);

// Runtime stubs.
static const char     *g_registered_method = NULL;
static ss_dispatch_fn_t g_registered_fn    = NULL;

void
n00b_rpc_register_server_stream(const char *method, ss_dispatch_fn_t fn)
{
    g_registered_method = method;
    g_registered_fn     = fn;
}

static Req_t g_decoded_req;

_generic_struct typeid("result", Req_t *)
_n00b_cbor_decode_Req_t(n00b_buffer_t *buf)
{
    g_decoded_req.x = buf ? buf->p : 0;
    return (_generic_struct typeid("result", Req_t *)){
        .is_ok = true, .ok = &g_decoded_req,
    };
}

static n00b_buffer_t g_enc_buf;
n00b_buffer_t *
n00b_cbor_encode(void *thing)
{
    Req_t *r        = thing;
    g_enc_buf.p     = r->x;
    return &g_enc_buf;
}

// Stub transport: invoke the dispatcher directly and echo the result.
_generic_struct typeid("result", _generic_struct typeid("rpc_stream", n00b_buffer_t *) *)
n00b_rpc_call_server_stream(n00b_rpc_ctx_t     *ctx,
                            n00b_rpc_channel_t *chan,
                            const char         *full_method,
                            n00b_buffer_t      *req_cbor)
{
    (void)chan;
    assert(g_registered_method != NULL);
    assert(strcmp(full_method, g_registered_method) == 0);
    return g_registered_fn(req_cbor, ctx);
}

// User handler — server-stream shape.
static _generic_struct typeid("rpc_stream", StreamItem) g_out_stream;

_generic_struct typeid("result", _generic_struct typeid("rpc_stream", StreamItem) *)
emit_items(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("a.b.S/Stream")
{
    (void)req;
    (void)ctx;
    return (_generic_struct typeid("result",
                                    _generic_struct typeid("rpc_stream", StreamItem) *)){
        .is_ok = true,
        .ok    = &g_out_stream,
    };
}

extern _generic_struct typeid("result", _generic_struct typeid("rpc_stream", StreamItem) *)
n00b_rpc_call_a_b_S__Stream(n00b_rpc_ctx_t     *ctx,
                            n00b_rpc_channel_t *chan,
                            Req_t              *req);

int
main(void)
{
    if (g_registered_method == NULL
        || strcmp(g_registered_method, "a.b.S/Stream") != 0) {
        fprintf(stderr, "FAIL: not registered (got %s)\n",
                g_registered_method ? g_registered_method : "(null)");
        return 1;
    }
    if (g_registered_fn == NULL) {
        return 2;
    }

    Req_t          req = {.x = 7};
    n00b_rpc_ctx_t ctx = {0};
    _generic_struct typeid("result",
                            _generic_struct typeid("rpc_stream", StreamItem) *) r
        = n00b_rpc_call_a_b_S__Stream(&ctx, NULL, &req);

    if (!r.is_ok) {
        return 3;
    }
    if (r.ok != &g_out_stream) {
        return 4;
    }

    printf("OK\n");
    return 0;
}
