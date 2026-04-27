#pragma once
/**
 * @file ncc_runtime.h
 * @brief Runtime support for code compiled by ncc.
 *
 * This header is included by ncc-compiled C output. It provides:
 * - ncc_vargs_t: variadic argument struct for the + parameter transform
 * - Rich string types for r"..." literals
 *
 * This does NOT include ncc's own build infrastructure — it is solely
 * for the output of ncc to link against.
 */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

    // ============================================================================
    // ncc_vargs_t — variadic argument packing
    // ============================================================================
    //
    // ncc's + transform rewrites:
    //   void foo(int x, +)   →  void foo(int x, ncc_vargs_t *_ncc_vargs)
    //   foo(1, a, b, c)      →  foo(1, &(ncc_vargs_t){3, 0, (void*[]){a,b,c}})

    typedef struct ncc_vargs_t ncc_vargs_t;

struct ncc_vargs_t {
    unsigned int nargs;
    unsigned int cur_ix;
    void       **args;
};

static inline unsigned int
ncc_remaining_vargs(ncc_vargs_t *vargs)
{
    if (!vargs) {
        return 0;
    }
    return vargs->nargs - vargs->cur_ix;
}

static inline void *
_ncc_vargs_next(ncc_vargs_t *vargs, bool *err)
{
    if (!vargs || vargs->cur_ix >= vargs->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return vargs->args[vargs->cur_ix++];
}

#define ncc_vargs_next(vargs) _ncc_vargs_next(vargs, nullptr)

static inline void *
_ncc_vargs_peek(ncc_vargs_t *vargs, bool *err)
{
    if (!vargs || vargs->cur_ix >= vargs->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return vargs->args[vargs->cur_ix];
}

#define ncc_vargs_peek(vargs) _ncc_vargs_peek(vargs, nullptr)

static inline void
ncc_vargs_advance(ncc_vargs_t *vargs)
{
    if (vargs && vargs->cur_ix < vargs->nargs) {
        ++vargs->cur_ix;
    }
}

static inline void
ncc_vargs_rewind(ncc_vargs_t *vargs)
{
    if (vargs) {
        vargs->cur_ix = 0;
    }
}

static inline void
ncc_vargs_advance_to_end(ncc_vargs_t *vargs)
{
    if (vargs) {
        vargs->cur_ix = vargs->nargs;
    }
}

static inline void **
ncc_get_next_vargs_by_address(ncc_vargs_t *vargs)
{
    if (!vargs || vargs->cur_ix >= vargs->nargs) {
        return nullptr;
    }
    return &vargs->args[vargs->cur_ix++];
}

static inline void *
ncc_vargs_peek_address(ncc_vargs_t *vargs)
{
    if (!vargs || vargs->cur_ix >= vargs->nargs) {
        return nullptr;
    }
    return &vargs->args[vargs->cur_ix];
}

static inline void *
ncc_vargs_peek_forward(ncc_vargs_t *vargs, unsigned int n, bool *err)
{
    if (!vargs || vargs->cur_ix + n < vargs->cur_ix) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    unsigned int ix = vargs->cur_ix + n;
    if (ix >= vargs->nargs) {
        if (err) {
            *err = true;
        }
        return nullptr;
    }
    if (err) {
        *err = false;
    }
    return vargs->args[ix];
}

static inline void **
ncc_vargs_peek_forward_address(ncc_vargs_t *vargs, unsigned int n)
{
    if (!vargs || vargs->cur_ix + n < vargs->cur_ix) {
        return nullptr;
    }
    unsigned int ix = vargs->cur_ix + n;
    if (ix >= vargs->nargs) {
        return nullptr;
    }
    return &vargs->args[ix];
}

// ============================================================================
// Rich string support types (for r"..." transform output)
// ============================================================================

#include "lib/style.h"

typedef struct ncc_string_t ncc_string_t;

struct ncc_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};

// ============================================================================
// ncc_string_to_ansi() — render styled ncc_string_t to ANSI escape sequences
// ============================================================================
//
// Implemented in string.c (libncc). Returns an allocated char * with embedded
// ANSI SGR escape sequences; caller frees.

extern char *ncc_string_to_ansi(const ncc_string_t *s);
