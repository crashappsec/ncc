#pragma once

#include "ncc.h"
#include "lib/alloc.h"

struct ncc_buffer_t {
    char  *data;
    size_t byte_len;
    size_t alloc_len;
};

static inline ncc_buffer_t *
ncc_buffer_empty(void)
{
    ncc_buffer_t *buf = ncc_alloc(ncc_buffer_t);
    return buf;
}

static inline ncc_buffer_t *
ncc_buffer_from_cstr(const char *s)
{
    ncc_buffer_t *buf = ncc_alloc(ncc_buffer_t);
    size_t len = strlen(s);

    buf->data      = (char *)ncc_alloc_size(1, len + 1);
    memcpy(buf->data, s, len);
    buf->byte_len  = len;
    buf->alloc_len = len + 1;

    return buf;
}

static inline ncc_buffer_t *
ncc_buffer_from_bytes(const char *data, int64_t len)
{
    ncc_buffer_t *buf = ncc_alloc(ncc_buffer_t);

    buf->data      = (char *)ncc_alloc_size(1, (size_t)len + 1);
    memcpy(buf->data, data, (size_t)len);
    buf->byte_len  = (size_t)len;
    buf->alloc_len = (size_t)len + 1;

    return buf;
}

static inline void
ncc_buffer_ensure(ncc_buffer_t *buf, size_t extra)
{
    size_t needed = buf->byte_len + extra;

    if (needed < buf->alloc_len) {
        return;
    }

    size_t new_alloc = buf->alloc_len ? buf->alloc_len : 64;

    while (new_alloc <= needed) {
        new_alloc *= 2;
    }

    buf->data      = (char *)ncc_realloc(buf->data, new_alloc);
    buf->alloc_len = new_alloc;
}

static inline void
ncc_buffer_append(ncc_buffer_t *buf, const char *data, size_t len)
{
    ncc_buffer_ensure(buf, len + 1);
    memcpy(buf->data + buf->byte_len, data, len);
    buf->byte_len += len;
    buf->data[buf->byte_len] = '\0';
}

static inline void
ncc_buffer_putc(ncc_buffer_t *buf, char c)
{
    ncc_buffer_ensure(buf, 2);
    buf->data[buf->byte_len++] = c;
    buf->data[buf->byte_len]   = '\0';
}

static inline void
ncc_buffer_puts(ncc_buffer_t *buf, const char *s)
{
    ncc_buffer_append(buf, s, strlen(s));
}

static inline char *
ncc_buffer_take(ncc_buffer_t *buf)
{
    char *result = buf->data;
    ncc_free(buf);
    return result;
}

static inline void
ncc_buffer_free(ncc_buffer_t *buf)
{
    if (!buf) {
        return;
    }

    ncc_free(buf->data);
    ncc_free(buf);
}
