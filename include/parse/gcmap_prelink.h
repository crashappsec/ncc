// gcmap_prelink.h -- link-time aggregation of n00b_gcraw records.
//
// With --ncc-gcmap-prelink, ncc emits per-TU GC type-map descriptors as flat,
// untyped uint64 records in the `n00b_gcraw` section (see xform_gc_typemap.c)
// so translation units carry no dependency on the codegen-ABI struct layouts.
// At link, ncc dumps every object's n00b_gcraw section, parses the records,
// dedups them by type hash, and generates one typed dictionary translation
// unit (n00b_gcmap_table / n00b_gcmap_count) that it compiles and links in.
//
// This header exposes the pure, self-contained core (parse + generate) so it
// can be unit-reasoned independently of the object-file/link plumbing. Record
// wire format (all words uint64, little-endian as laid down by the compiler):
//
//   [record_words, kind=1, type_hash, stride_words, n_fixed,
//    <fixed offsets...>, n_variants,
//    per variant { selector_offset, n_arms,
//                  per arm { selector, n_offsets, <offsets...> } } ]
//
// record_words counts every word in the record, including itself, so a reader
// can walk a concatenated section linearly without external boundaries.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define NCC_GCRAW_KIND_TYPEMAP 1u

typedef struct {
    uint64_t  selector;
    uint64_t  n_offsets;
    uint64_t *offsets; // element-relative word offsets
} ncc_gcraw_arm_t;

typedef struct {
    uint64_t         selector_offset; // word offset of the selector field
    uint64_t         n_arms;
    ncc_gcraw_arm_t *arms;
} ncc_gcraw_variant_t;

typedef struct {
    uint64_t             type_hash;
    uint64_t             stride;   // element stride in words
    uint64_t             n_fixed;
    uint64_t            *fixed_offsets; // unconditional pointer word offsets
    uint64_t             n_variants;
    ncc_gcraw_variant_t *variants;
} ncc_gcraw_record_t;

typedef struct {
    ncc_gcraw_record_t *records;
    size_t              len;
    size_t              cap;
} ncc_gcraw_set_t;

// Parse a concatenated n00b_gcraw byte stream (as dumped from an object's
// section) into `set`, appending records whose type_hash has not been seen yet
// (first writer wins; later duplicates are dropped). Safe against truncation
// and malformed counts: on any inconsistency it sets *err and returns false
// without reading out of bounds. `set` may already hold records from earlier
// objects; dedup is across the whole set. Returns true on success.
bool ncc_gcraw_parse(const uint8_t *bytes,
                     size_t         nbytes,
                     ncc_gcraw_set_t *set,
                     char           **err_out);

// Generate the typed dictionary translation unit (a malloc'd C string the
// caller frees) from the deduped record set: an `n00b_gc_type_map_entry_t
// n00b_gcmap_table[]` sorted by type_hash plus `n00b_gcmap_count`. Includes
// "core/codegen_abi.h"; emits integer offset literals (no offsetof), so it
// needs no aggregate type definitions.
char *ncc_gcraw_generate_c(const ncc_gcraw_set_t *set);

void ncc_gcraw_set_free(ncc_gcraw_set_t *set);
