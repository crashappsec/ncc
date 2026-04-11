#pragma once

/**
 * @file arena.h
 * @brief Simple bump allocator for batch-lifetime allocations.
 *
 * Allocations are 16-byte aligned. There is no per-object free.
 * Call ncc_arena_reset() to reclaim all allocations (blocks are kept
 * for reuse) or ncc_arena_free() to return everything to the OS.
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define NCC_ARENA_DEFAULT_BLOCK_SIZE (256 * 1024)
#define NCC_ARENA_ALIGNMENT          16

typedef struct ncc_arena_block_t {
    struct ncc_arena_block_t *next;
    size_t                    capacity;
    size_t                    used;
    alignas(NCC_ARENA_ALIGNMENT) char data[];
} ncc_arena_block_t;

typedef struct {
    ncc_arena_block_t *current;
    ncc_arena_block_t *first;
    size_t             default_block_size;
#ifdef NCC_MEM_DEBUG
    size_t             total_allocated;
#endif
} ncc_arena_t;

static inline void
ncc_arena_init(ncc_arena_t *a, size_t block_size)
{
    *a = (ncc_arena_t){
        .default_block_size = block_size ? block_size
                                         : NCC_ARENA_DEFAULT_BLOCK_SIZE,
    };
}

static inline size_t
_ncc_arena_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

static inline void *
ncc_arena_alloc(ncc_arena_t *a, size_t size)
{
    size = _ncc_arena_align_up(size, NCC_ARENA_ALIGNMENT);

    // Try current block.
    if (a->current && a->current->used + size <= a->current->capacity) {
        void *ptr = a->current->data + a->current->used;
        a->current->used += size;
#ifdef NCC_MEM_DEBUG
        a->total_allocated += size;
#endif
        return memset(ptr, 0, size);
    }

    // Try reusing the next block in the chain (from a previous reset).
    if (a->current && a->current->next
        && a->current->next->capacity >= size) {
        a->current = a->current->next;
        a->current->used = size;
#ifdef NCC_MEM_DEBUG
        a->total_allocated += size;
#endif
        return memset(a->current->data, 0, size);
    }

    // Allocate a new block.
    size_t block_cap = a->default_block_size;
    if (size > block_cap) {
        block_cap = size;
    }

    ncc_arena_block_t *block = (ncc_arena_block_t *)malloc(
        sizeof(ncc_arena_block_t) + block_cap);
    if (!block) {
        return NULL;
    }

    block->capacity = block_cap;
    block->used     = size;
    block->next     = NULL;

    if (a->current) {
        // Insert after current, preserving any tail chain for future resets.
        block->next      = a->current->next;
        a->current->next = block;
    }
    else {
        a->first = block;
    }

    a->current = block;
#ifdef NCC_MEM_DEBUG
    a->total_allocated += size;
#endif

    return memset(block->data, 0, size);
}

/**
 * @brief Reset arena: keep all blocks but mark them empty.
 *
 * After reset, subsequent allocations reuse existing blocks.
 */
static inline void
ncc_arena_reset(ncc_arena_t *a)
{
    for (ncc_arena_block_t *b = a->first; b; b = b->next) {
        b->used = 0;
    }

    a->current = a->first;
#ifdef NCC_MEM_DEBUG
    a->total_allocated = 0;
#endif
}

/**
 * @brief Free arena: return all blocks to the OS.
 */
static inline void
ncc_arena_free(ncc_arena_t *a)
{
    ncc_arena_block_t *b = a->first;

    while (b) {
        ncc_arena_block_t *next = b->next;
        free(b);
        b = next;
    }

    *a = (ncc_arena_t){0};
}

#ifdef NCC_MEM_DEBUG
/**
 * @brief Return total bytes allocated from this arena (since last reset).
 */
static inline size_t
ncc_arena_total_allocated(const ncc_arena_t *a)
{
    return a->total_allocated;
}
#endif
