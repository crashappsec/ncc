// test_rpc_bidi — Phase E bidirectional-stream end-to-end.
//
// Handler takes a typed inbound stream and returns a typed
// outbound stream. Dispatcher via `n00b_rpc_register_bidi`,
// client stub via `n00b_rpc_call_bidi`.

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int n00b_err_t;
typedef struct { int p; } n00b_buffer_t;
typedef struct { int p; } n00b_rpc_ctx_t;
typedef struct { int p; } n00b_rpc_channel_t;
typedef struct { int x; } ChatIn;
typedef struct { int y; } ChatOut;

_generic_struct typeid("rpc_stream", n00b_buffer_t *) { void *_opaque; };
_generic_struct typeid("rpc_stream", ChatIn)          { void *_opaque; };
_generic_struct typeid("rpc_stream", ChatOut)         { void *_opaque; };

_generic_struct typeid("result", _generic_struct typeid("rpc_stream", ChatOut) *) {
    bool is_ok;
    union {
        _generic_struct typeid("rpc_stream", ChatOut) *ok;
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

typedef _generic_struct typeid("result",
                                _generic_struct typeid("rpc_stream", n00b_buffer_t *) *)
    (*bd_dispatch_fn_t)(_generic_struct typeid("rpc_stream", n00b_buffer_t *) *,
                        n00b_rpc_ctx_t *);

static const char    *g_registered_method = NULL;
static bd_dispatch_fn_t g_registered_fn   = NULL;

void
n00b_rpc_register_bidi(const char *method, bd_dispatch_fn_t fn)
{
    g_registered_method = method;
    g_registered_fn     = fn;
}

_generic_struct typeid("result", _generic_struct typeid("rpc_stream", n00b_buffer_t *) *)
n00b_rpc_call_bidi(n00b_rpc_ctx_t     *ctx,
                   n00b_rpc_channel_t *chan,
                   const char         *full_method,
                   _generic_struct typeid("rpc_stream", n00b_buffer_t *) *in)
{
    (void)chan;
    assert(g_registered_method != NULL);
    assert(strcmp(full_method, g_registered_method) == 0);
    return g_registered_fn(in, ctx);
}

static _generic_struct typeid("rpc_stream", ChatOut) g_out_stream;

_generic_struct typeid("result", _generic_struct typeid("rpc_stream", ChatOut) *)
chat(_generic_struct typeid("rpc_stream", ChatIn) *in, n00b_rpc_ctx_t *ctx)
    @rpc("a.b.C/Chat")
{
    (void)in;
    (void)ctx;
    return (_generic_struct typeid("result",
                                    _generic_struct typeid("rpc_stream", ChatOut) *)){
        .is_ok = true,
        .ok    = &g_out_stream,
    };
}

extern _generic_struct typeid("result", _generic_struct typeid("rpc_stream", ChatOut) *)
n00b_rpc_call_a_b_C__Chat(n00b_rpc_ctx_t                                       *ctx,
                          n00b_rpc_channel_t                                   *chan,
                          _generic_struct typeid("rpc_stream", ChatIn)         *in);

int
main(void)
{
    if (g_registered_method == NULL
        || strcmp(g_registered_method, "a.b.C/Chat") != 0) {
        return 1;
    }
    if (g_registered_fn == NULL) {
        return 2;
    }

    _generic_struct typeid("rpc_stream", ChatIn) in_stream;
    n00b_rpc_ctx_t ctx = {0};

    _generic_struct typeid("result",
                            _generic_struct typeid("rpc_stream", ChatOut) *) r
        = n00b_rpc_call_a_b_C__Chat(&ctx, NULL, &in_stream);

    if (!r.is_ok) {
        return 3;
    }
    if (r.ok != &g_out_stream) {
        return 4;
    }

    printf("OK\n");
    return 0;
}
