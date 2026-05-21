// test_rpc_client_stream — Phase E client-stream end-to-end.
//
// Handler takes a typed inbound stream and returns a single
// CBOR-encodable response. Dispatcher registered via
// `n00b_rpc_register_client_stream`. Client stub goes through
// `n00b_rpc_call_client_stream`.

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int n00b_err_t;
typedef struct { int p; } n00b_buffer_t;
typedef struct { int p; } n00b_rpc_ctx_t;
typedef struct { int p; } n00b_rpc_channel_t;
typedef struct { int x; } Chunk;
typedef struct { int n; } UploadReply;

_generic_struct typeid("rpc_stream", n00b_buffer_t *) { void *_opaque; };
_generic_struct typeid("rpc_stream", Chunk)           { void *_opaque; };

_generic_struct typeid("result", UploadReply *) {
    bool is_ok; union { UploadReply *ok; n00b_err_t err; };
};
_generic_struct typeid("result", n00b_buffer_t *) {
    bool is_ok; union { n00b_buffer_t *ok; n00b_err_t err; };
};

typedef _generic_struct typeid("result", n00b_buffer_t *)
    (*cs_dispatch_fn_t)(_generic_struct typeid("rpc_stream", n00b_buffer_t *) *,
                        n00b_rpc_ctx_t *);

static const char     *g_registered_method = NULL;
static cs_dispatch_fn_t g_registered_fn    = NULL;

void
n00b_rpc_register_client_stream(const char *method, cs_dispatch_fn_t fn)
{
    g_registered_method = method;
    g_registered_fn     = fn;
}

static UploadReply g_decoded_reply;

_generic_struct typeid("result", UploadReply *)
typeid("cbor_decode", UploadReply *)(n00b_buffer_t *buf)
{
    g_decoded_reply.n = buf ? buf->p : 0;
    return (_generic_struct typeid("result", UploadReply *)){
        .is_ok = true, .ok = &g_decoded_reply,
    };
}

static n00b_buffer_t g_enc_buf;
n00b_buffer_t *
typeid("cbor_encode", UploadReply *)(UploadReply *thing)
{
    g_enc_buf.p    = thing->n;
    return &g_enc_buf;
}

_generic_struct typeid("result", n00b_buffer_t *)
n00b_rpc_call_client_stream(n00b_rpc_ctx_t     *ctx,
                            n00b_rpc_channel_t *chan,
                            const char         *full_method,
                            _generic_struct typeid("rpc_stream", n00b_buffer_t *) *in)
{
    (void)chan;
    assert(g_registered_method != NULL);
    assert(strcmp(full_method, g_registered_method) == 0);
    return g_registered_fn(in, ctx);
}

static UploadReply g_user_reply = {.n = 99};

_generic_struct typeid("result", UploadReply *)
upload(_generic_struct typeid("rpc_stream", Chunk) *in, n00b_rpc_ctx_t *ctx)
    @rpc("a.b.U/Upload")
{
    (void)in;
    (void)ctx;
    return (_generic_struct typeid("result", UploadReply *)){
        .is_ok = true, .ok = &g_user_reply,
    };
}

extern _generic_struct typeid("result", UploadReply *)
n00b_rpc_call_a_b_U__Upload(n00b_rpc_ctx_t                                       *ctx,
                            n00b_rpc_channel_t                                   *chan,
                            _generic_struct typeid("rpc_stream", Chunk)          *in);

int
main(void)
{
    if (g_registered_method == NULL
        || strcmp(g_registered_method, "a.b.U/Upload") != 0) {
        return 1;
    }
    if (g_registered_fn == NULL) {
        return 2;
    }

    _generic_struct typeid("rpc_stream", Chunk) in_stream;
    n00b_rpc_ctx_t ctx = {0};

    _generic_struct typeid("result", UploadReply *) r
        = n00b_rpc_call_a_b_U__Upload(&ctx, NULL, &in_stream);

    if (!r.is_ok) {
        return 3;
    }
    if (r.ok->n != 99) {
        fprintf(stderr, "FAIL: expected n=99, got %d\n", r.ok->n);
        return 4;
    }

    printf("OK\n");
    return 0;
}
