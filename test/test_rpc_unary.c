// test_rpc_unary — Phase D end-to-end: an @rpc-annotated unary
// handler compiles + runs through ncc. Verifies:
//   - the constructor registered the dispatcher under the right
//     method string,
//   - the generated dispatcher decodes / dispatches / encodes,
//   - the generated client stub round-trips against a stubbed
//     transport.
//
// Uses raw ncc constructs (`typeid`, `_generic_struct`) directly —
// libn00b provides macros that expand to this form, but the test
// runs without libn00b headers.

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int n00b_err_t;
typedef struct { int placeholder; } n00b_buffer_t;
typedef struct { int placeholder; } n00b_rpc_ctx_t;
typedef struct { int placeholder; } n00b_rpc_channel_t;

typedef struct { int x; } Req_t;
typedef struct { int y; } Reply_t;

// Result-type bodies. ncc's `_generic_struct` deduplicates these
// across the TU; the dispatcher / stub emitted by ncc references
// them by tag.
_generic_struct typeid("result", Req_t *) {
    bool is_ok;
    union { Req_t *ok; n00b_err_t err; };
};
_generic_struct typeid("result", Reply_t *) {
    bool is_ok;
    union { Reply_t *ok; n00b_err_t err; };
};
_generic_struct typeid("result", n00b_buffer_t *) {
    bool is_ok;
    union { n00b_buffer_t *ok; n00b_err_t err; };
};

// Dispatcher function-pointer type — matches what ncc's emitted
// `_n00b_rpc_dispatch__*` will look like.
typedef _generic_struct typeid("result", n00b_buffer_t *)
    (*dispatch_fn_t)(n00b_buffer_t *, n00b_rpc_ctx_t *);

// --- Runtime stubs ---

static const char *g_registered_method = NULL;
static dispatch_fn_t g_registered_fn   = NULL;

void
n00b_rpc_register(const char *method, dispatch_fn_t fn)
{
    g_registered_method = method;
    g_registered_fn     = fn;
}

// The dispatcher receives a CBOR-encoded request buffer and returns a
// CBOR-encoded response buffer (or err). We use the buffer's
// `placeholder` field as a stand-in payload.
static Req_t g_last_req_seen;

_generic_struct typeid("result", Req_t *)
typeid("cbor_decode", Req_t *)(n00b_buffer_t *buf)
{
    g_last_req_seen.x = buf ? buf->placeholder : -1;
    return (_generic_struct typeid("result", Req_t *)){
        .is_ok = true,
        .ok    = &g_last_req_seen,
    };
}

static Reply_t g_decoded_reply;

_generic_struct typeid("result", Reply_t *)
typeid("cbor_decode", Reply_t *)(n00b_buffer_t *buf)
{
    g_decoded_reply.y = buf ? buf->placeholder : -1;
    return (_generic_struct typeid("result", Reply_t *)){
        .is_ok = true,
        .ok    = &g_decoded_reply,
    };
}

static n00b_buffer_t g_encoded_buf;

n00b_buffer_t *
typeid("cbor_encode", Req_t *)(Req_t *thing)
{
    g_encoded_buf.placeholder = thing->x;
    return &g_encoded_buf;
}

n00b_buffer_t *
typeid("cbor_encode", Reply_t *)(Reply_t *thing)
{
    g_encoded_buf.placeholder = thing->y;
    return &g_encoded_buf;
}

// Stubbed transport: echoes the request buffer as the response.
// Real n00b_rpc_call_unary issues an H3 POST and awaits the reply;
// here we just route through the registered dispatcher directly so
// the test exercises both the client stub and the dispatcher in one
// run.
_generic_struct typeid("result", n00b_buffer_t *)
n00b_rpc_call_unary(n00b_rpc_ctx_t     *ctx,
                    n00b_rpc_channel_t *chan,
                    const char         *full_method,
                    n00b_buffer_t      *req_cbor)
{
    (void)chan;
    assert(g_registered_method != NULL);
    assert(strcmp(full_method, g_registered_method) == 0);
    return g_registered_fn(req_cbor, ctx);
}

// --- User handler with @rpc annotation ---

_generic_struct typeid("result", Reply_t *)
hello(Req_t *req, n00b_rpc_ctx_t *ctx) @rpc("a.b.C/D")
{
    (void)ctx;
    static Reply_t r;
    r.y = req->x * 2;
    return (_generic_struct typeid("result", Reply_t *)){
        .is_ok = true,
        .ok    = &r,
    };
}

// --- Forward declarations for the ncc-emitted stubs ---

extern _generic_struct typeid("result", Reply_t *)
n00b_rpc_call_a_b_C__D(n00b_rpc_ctx_t     *ctx,
                       n00b_rpc_channel_t *chan,
                       Req_t              *req);

int
main(void)
{
    // Constructor should have fired at process start.
    if (g_registered_method == NULL
        || strcmp(g_registered_method, "a.b.C/D") != 0) {
        fprintf(stderr, "FAIL: dispatcher not registered as a.b.C/D "
                        "(got %s)\n",
                g_registered_method ? g_registered_method : "(null)");
        return 1;
    }

    if (g_registered_fn == NULL) {
        fprintf(stderr, "FAIL: dispatcher function pointer is null\n");
        return 2;
    }

    // Round-trip through the generated client stub.
    Req_t req = {.x = 21};
    n00b_rpc_ctx_t ctx = {0};

    _generic_struct typeid("result", Reply_t *) reply =
        n00b_rpc_call_a_b_C__D(&ctx, NULL, &req);

    if (!reply.is_ok) {
        fprintf(stderr, "FAIL: client stub returned err\n");
        return 3;
    }

    if (reply.ok->y != 42) {
        fprintf(stderr, "FAIL: expected y=42, got y=%d\n", reply.ok->y);
        return 4;
    }

    printf("OK\n");
    return 0;
}
