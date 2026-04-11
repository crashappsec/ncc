#pragma once

#include "ncc.h"
#include "lib/macros.h"
#include "lib/align.h"

// ============================================================================
// Memory debug counters (enabled by -DNCC_MEM_DEBUG)
// ============================================================================

#ifdef NCC_MEM_DEBUG

#include <stdio.h>
#include <stdatomic.h>

typedef struct {
    _Atomic size_t total_allocs;
    _Atomic size_t total_frees;
    _Atomic size_t total_bytes_requested;
    _Atomic size_t total_bytes_freed;
    _Atomic size_t live_bytes;
    _Atomic size_t peak_live_bytes;
} ncc_mem_counters_t;

extern ncc_mem_counters_t ncc_mem_counters;

static inline void
ncc_mem_track_alloc(size_t bytes)
{
    atomic_fetch_add_explicit(&ncc_mem_counters.total_allocs, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&ncc_mem_counters.total_bytes_requested, bytes,
                              memory_order_relaxed);
    size_t live = atomic_fetch_add_explicit(&ncc_mem_counters.live_bytes, bytes,
                                            memory_order_relaxed) + bytes;
    size_t peak = atomic_load_explicit(&ncc_mem_counters.peak_live_bytes,
                                       memory_order_relaxed);
    while (live > peak) {
        if (atomic_compare_exchange_weak_explicit(
                &ncc_mem_counters.peak_live_bytes, &peak, live,
                memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }
}

static inline void
ncc_mem_track_free(size_t bytes)
{
    atomic_fetch_add_explicit(&ncc_mem_counters.total_frees, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&ncc_mem_counters.total_bytes_freed, bytes,
                              memory_order_relaxed);
    atomic_fetch_sub_explicit(&ncc_mem_counters.live_bytes, bytes,
                              memory_order_relaxed);
}

static inline void
ncc_mem_report(const char *label)
{
    fprintf(stderr, "[mem] %-20s  allocs=%-8zu frees=%-8zu "
            "live=%-10zu peak=%-10zu\n",
            label,
            atomic_load_explicit(&ncc_mem_counters.total_allocs,
                                 memory_order_relaxed),
            atomic_load_explicit(&ncc_mem_counters.total_frees,
                                 memory_order_relaxed),
            atomic_load_explicit(&ncc_mem_counters.live_bytes,
                                 memory_order_relaxed),
            atomic_load_explicit(&ncc_mem_counters.peak_live_bytes,
                                 memory_order_relaxed));
}

// Tracked header: magic + size, so free can recover the size and detect
// whether a pointer was allocated through our tracked path.
#define _NCC_TRACK_MAGIC ((size_t)0xA110CA110CA110CCULL)

typedef struct {
    size_t magic;
    size_t size;
} _ncc_track_header_t;

static inline void *
_ncc_tracked_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    _ncc_track_header_t *block = (_ncc_track_header_t *)calloc(
        1, sizeof(_ncc_track_header_t) + total);

    if (block) {
        block->magic = _NCC_TRACK_MAGIC;
        block->size  = total;
        ncc_mem_track_alloc(total);
        return (char *)block + sizeof(_ncc_track_header_t);
    }

    return nullptr;
}

static inline void *
_ncc_tracked_realloc(void *ptr, size_t new_size)
{
    if (!ptr) {
        return _ncc_tracked_calloc(1, new_size);
    }

    _ncc_track_header_t *old_hdr =
        (_ncc_track_header_t *)((char *)ptr - sizeof(_ncc_track_header_t));

    if (old_hdr->magic != _NCC_TRACK_MAGIC) {
        // Not our allocation (e.g. strdup, realpath) — fall back to system.
        return realloc(ptr, new_size);
    }

    size_t old_size = old_hdr->size;
    _ncc_track_header_t *new_hdr = (_ncc_track_header_t *)realloc(
        old_hdr, sizeof(_ncc_track_header_t) + new_size);

    if (new_hdr) {
        ncc_mem_track_free(old_size);
        new_hdr->size = new_size;
        ncc_mem_track_alloc(new_size);
        return (char *)new_hdr + sizeof(_ncc_track_header_t);
    }

    return nullptr;
}

static inline void
_ncc_tracked_free(void *ptr)
{
    if (!ptr) {
        return;
    }

    _ncc_track_header_t *hdr =
        (_ncc_track_header_t *)((char *)ptr - sizeof(_ncc_track_header_t));

    if (hdr->magic != _NCC_TRACK_MAGIC) {
        // Not our allocation — use system free.
        free(ptr);
        return;
    }

    ncc_mem_track_free(hdr->size);
    hdr->magic = 0;  // Poison to catch double-free.
    free(hdr);
}

#define ncc_alloc(T, ...) \
    ((T *)_ncc_tracked_calloc(1, sizeof(T)))

#define ncc_alloc_array(T, N, ...) \
    ((T *)_ncc_tracked_calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size(n, sz, ...) \
    _ncc_tracked_calloc((size_t)(n) ? (size_t)(n) : 1, \
                        (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_flex(T1, T2, N2, ...) \
    _ncc_tracked_calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void *
ncc_realloc(void *ptr, size_t sz)
{
    return _ncc_tracked_realloc(ptr, sz);
}

static inline void
ncc_free(void *ptr)
{
    _ncc_tracked_free(ptr);
}

#else // !NCC_MEM_DEBUG

// Core allocation: zero-filled.
#define ncc_alloc(T, ...) \
    ((T *)calloc(1, sizeof(T)))

#define ncc_alloc_array(T, N, ...) \
    ((T *)calloc((size_t)(N) ? (size_t)(N) : 1, sizeof(T)))

#define ncc_alloc_size(n, sz, ...) \
    calloc((size_t)(n) ? (size_t)(n) : 1, (size_t)(sz) ? (size_t)(sz) : 1)

#define ncc_alloc_flex(T1, T2, N2, ...) \
    calloc(1, sizeof(T1) + sizeof(T2) * (size_t)(N2))

static inline void *
ncc_realloc(void *ptr, size_t sz)
{
    return realloc(ptr, sz);
}

static inline void
ncc_free(void *ptr)
{
    free(ptr);
}

#endif // NCC_MEM_DEBUG
