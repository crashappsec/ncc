#pragma once
/**
 * @file dict.h
 * @brief Simple open-addressing hash table (standalone extraction).
 *
 * Replaces the n00b lock-free robin-hood dictionary with a plain
 * single-threaded open-addressing table using linear probing.
 * Stores void* keys and void* values.
 */

#include <stdint.h>
#include <string.h>
#include "ncc.h"
#include "lib/alloc.h"
#include "lib/hash.h"

#define NCC_DICT_MIN_SIZE_LOG 4
#define NCC_DICT_MIN_SIZE     (1 << NCC_DICT_MIN_SIZE_LOG)

// Load factor threshold: resize when count > capacity * 3/4.
#define _NCC_DICT_LOAD_NUM 3
#define _NCC_DICT_LOAD_DEN 4

typedef int (*ncc_dict_sort_fn)(const void *, const void *);

// Bucket states.
#define _NCC_BUCKET_EMPTY    0
#define _NCC_BUCKET_OCCUPIED 1
#define _NCC_BUCKET_DELETED  2

typedef struct {
    void    *key;
    void    *value;
    uint8_t  state;
} ncc_dict_bucket_t;

typedef bool (*ncc_dict_key_eq_fn)(const void *, const void *);

struct ncc_dict_t {
    ncc_hash_fn         hash_fn;
    ncc_dict_key_eq_fn  key_eq;
    ncc_dict_bucket_t  *buckets;
    size_t               capacity; // Always a power of 2.
    size_t               count;    // Number of OCCUPIED entries.
    size_t               tombstones;
};

// Compat: n00b uses ncc_dict_item_t in iteration.
typedef struct {
    void    *key;
    void    *value;
    uint64_t order;
} ncc_dict_item_t;

// ============================================================================
// Internal helpers
// ============================================================================

static inline size_t
_ncc_dict_mask(const ncc_dict_t *d)
{
    return d->capacity - 1;
}

static inline size_t
_ncc_dict_probe(ncc_hash_value_t hv, size_t mask)
{
    return (size_t)(hv & (ncc_hash_value_t)mask);
}

// Forward declaration for resize.
static inline void _ncc_dict_resize(ncc_dict_t *d, size_t new_cap);

static inline void
_ncc_dict_maybe_resize(ncc_dict_t *d)
{
    // Resize when occupied + tombstones exceed 3/4 capacity.
    if ((d->count + d->tombstones) * _NCC_DICT_LOAD_DEN
        > d->capacity * _NCC_DICT_LOAD_NUM) {
        // If many tombstones, rehash at same size; else double.
        size_t new_cap = d->capacity;
        if (d->count * _NCC_DICT_LOAD_DEN > d->capacity * _NCC_DICT_LOAD_NUM) {
            new_cap = d->capacity * 2;
        }
        _ncc_dict_resize(d, new_cap);
    }
}

static inline void
_ncc_dict_resize(ncc_dict_t *d, size_t new_cap)
{
    ncc_dict_bucket_t *old_buckets = d->buckets;
    size_t              old_cap     = d->capacity;

    d->buckets    = (ncc_dict_bucket_t *)ncc_alloc_array(ncc_dict_bucket_t, new_cap);
    d->capacity   = new_cap;
    d->count      = 0;
    d->tombstones = 0;

    size_t mask = _ncc_dict_mask(d);

    for (size_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].state != _NCC_BUCKET_OCCUPIED) {
            continue;
        }
        ncc_hash_value_t hv  = d->hash_fn(old_buckets[i].key);
        size_t            idx = _ncc_dict_probe(hv, mask);

        while (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED) {
            idx = (idx + 1) & mask;
        }

        d->buckets[idx].key   = old_buckets[i].key;
        d->buckets[idx].value = old_buckets[i].value;
        d->buckets[idx].state = _NCC_BUCKET_OCCUPIED;
        d->count++;
    }

    ncc_free(old_buckets);
}

// ============================================================================
// Default key equality (pointer identity).
// ============================================================================

static inline bool
_ncc_dict_ptr_eq(const void *a, const void *b)
{
    return a == b;
}

// C-string key equality.
static inline bool
ncc_dict_cstr_eq(const void *a, const void *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return strcmp((const char *)a, (const char *)b) == 0;
}

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize an untyped dictionary.
 *
 * @param dict    Dictionary to initialize.
 * @param hash_fn Hash function for keys (nullptr = ncc_hash_word).
 * @param key_eq  Key equality function (nullptr = pointer identity).
 */
static inline void
ncc_dict_init(ncc_dict_t *dict,
                       ncc_hash_fn         hash_fn,
                       ncc_dict_key_eq_fn  key_eq)
{
    dict->hash_fn    = hash_fn ? hash_fn : ncc_hash_word;
    dict->key_eq     = key_eq  ? key_eq  : _ncc_dict_ptr_eq;
    dict->capacity   = NCC_DICT_MIN_SIZE;
    dict->count      = 0;
    dict->tombstones = 0;
    dict->buckets    = (ncc_dict_bucket_t *)ncc_alloc_array(
        ncc_dict_bucket_t, NCC_DICT_MIN_SIZE);
}

/**
 * @brief Insert or update a key/value pair.
 * @return The previous value, or nullptr if newly inserted.
 */
static inline void *
_ncc_dict_put(ncc_dict_t *d, void *key, void *value)
{
    _ncc_dict_maybe_resize(d);

    ncc_hash_value_t hv   = d->hash_fn(key);
    size_t            mask = _ncc_dict_mask(d);
    size_t            idx  = _ncc_dict_probe(hv, mask);
    size_t            first_deleted = (size_t)-1;

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            // Not found — insert.
            size_t ins = (first_deleted != (size_t)-1) ? first_deleted : idx;
            if (d->buckets[ins].state == _NCC_BUCKET_DELETED) {
                d->tombstones--;
            }
            d->buckets[ins].key   = key;
            d->buckets[ins].value = value;
            d->buckets[ins].state = _NCC_BUCKET_OCCUPIED;
            d->count++;
            return nullptr;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_DELETED) {
            if (first_deleted == (size_t)-1) {
                first_deleted = idx;
            }
        }
        else if (d->key_eq(d->buckets[idx].key, key)) {
            // Found — update.
            void *old             = d->buckets[idx].value;
            d->buckets[idx].value = value;
            return old;
        }

        idx = (idx + 1) & mask;
    }
}

/**
 * @brief Look up a key.
 * @return The value, or nullptr if not found. Sets *found accordingly.
 */
static inline void *
_ncc_dict_get(ncc_dict_t *d, void *key, bool *found)
{
    ncc_hash_value_t hv   = d->hash_fn(key);
    size_t            mask = _ncc_dict_mask(d);
    size_t            idx  = _ncc_dict_probe(hv, mask);

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            if (found) {
                *found = false;
            }
            return nullptr;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED
            && d->key_eq(d->buckets[idx].key, key)) {
            if (found) {
                *found = true;
            }
            return d->buckets[idx].value;
        }

        idx = (idx + 1) & mask;
    }
}

/**
 * @brief Remove a key. Returns true if found and removed.
 */
static inline bool
_ncc_dict_remove(ncc_dict_t *d, void *key)
{
    ncc_hash_value_t hv   = d->hash_fn(key);
    size_t            mask = _ncc_dict_mask(d);
    size_t            idx  = _ncc_dict_probe(hv, mask);

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            return false;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED
            && d->key_eq(d->buckets[idx].key, key)) {
            d->buckets[idx].state = _NCC_BUCKET_DELETED;
            d->count--;
            d->tombstones++;
            return true;
        }

        idx = (idx + 1) & mask;
    }
}

/**
 * @brief Insert only if key is absent. Returns false if key exists.
 */
static inline bool
_ncc_dict_add(ncc_dict_t *d, void *key, void *value)
{
    bool found;
    _ncc_dict_get(d, key, &found);

    if (found) {
        return false;
    }

    _ncc_dict_put(d, key, value);
    return true;
}

/**
 * @brief Replace an existing key's value. Returns false if key absent.
 */
static inline bool
_ncc_dict_replace(ncc_dict_t *d, void *key, void *value)
{
    ncc_hash_value_t hv   = d->hash_fn(key);
    size_t            mask = _ncc_dict_mask(d);
    size_t            idx  = _ncc_dict_probe(hv, mask);

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            return false;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED
            && d->key_eq(d->buckets[idx].key, key)) {
            d->buckets[idx].value = value;
            return true;
        }

        idx = (idx + 1) & mask;
    }
}

// Macro wrappers matching the n00b API (cast through void*).
#define ncc_dict_put(d, k, v) \
    _ncc_dict_put(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_get(d, k, b) \
    _ncc_dict_get(d, ((void *)(uintptr_t)(k)), (b))
#define ncc_dict_replace(d, k, v) \
    _ncc_dict_replace(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_add(d, k, v) \
    _ncc_dict_add(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_remove(d, k) \
    _ncc_dict_remove(d, ((void *)(uintptr_t)(k)))

/**
 * @brief Check whether a key exists in the dictionary.
 */
static inline bool
ncc_dict_contains(ncc_dict_t *d, void *v)
{
    bool found;
    ncc_dict_get(d, v, &found);
    return found;
}

/**
 * @brief Free the dictionary's internal storage.
 */
static inline void
ncc_dict_free(ncc_dict_t *d)
{
    ncc_free(d->buckets);
    d->buckets  = nullptr;
    d->capacity = 0;
    d->count    = 0;
}

/**
 * @brief Return the number of entries in the dictionary.
 */
static inline size_t
ncc_dict_len(ncc_dict_t *d)
{
    return d->count;
}
