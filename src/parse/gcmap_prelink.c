// gcmap_prelink.c -- parse n00b_gcraw records and generate the typed GC
// type-map dictionary translation unit. See parse/gcmap_prelink.h for the
// record wire format. This file is the pure core (no object-file / link
// plumbing) so its bounds-safety can be reasoned about in isolation: every
// read is checked against the record's own declared end and the section end,
// so a malformed or truncated section is rejected, never read out of bounds.

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/gcmap_prelink.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void
gcraw_set_err(char **err_out, const char *fmt, ...)
{
    if (!err_out || *err_out) {
        return; // first error wins
    }
    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;
    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);
    *err_out = ncc_buffer_take(buf);
}

static bool
set_has(const ncc_gcraw_set_t *set, uint64_t hash)
{
    for (size_t i = 0; i < set->len; i++) {
        if (set->records[i].type_hash == hash) {
            return true;
        }
    }
    return false;
}

static void
set_append(ncc_gcraw_set_t *set, ncc_gcraw_record_t rec)
{
    if (set->len == set->cap) {
        size_t ncap   = set->cap ? set->cap * 2 : 16;
        set->records  = ncc_realloc(set->records,
                                   ncap * sizeof(ncc_gcraw_record_t));
        set->cap      = ncap;
    }
    set->records[set->len++] = rec;
}

// Copy `n` words from w[at..at+n) into a fresh array (NULL if n == 0).
static uint64_t *
dup_words(const uint64_t *w, size_t at, uint64_t n)
{
    if (n == 0) {
        return nullptr;
    }
    uint64_t *out = ncc_alloc_array(uint64_t, (size_t)n);
    memcpy(out, w + at, (size_t)n * sizeof(uint64_t));
    return out;
}

static void
free_record(ncc_gcraw_record_t *r)
{
    ncc_free(r->fixed_offsets);
    for (uint64_t v = 0; v < r->n_variants; v++) {
        for (uint64_t a = 0; a < r->variants[v].n_arms; a++) {
            ncc_free(r->variants[v].arms[a].offsets);
        }
        ncc_free(r->variants[v].arms);
    }
    ncc_free(r->variants);
    memset(r, 0, sizeof(*r));
}

bool
ncc_gcraw_parse(const uint8_t   *bytes,
                size_t           nbytes,
                ncc_gcraw_set_t *set,
                char           **err_out)
{
    if (nbytes == 0) {
        return true;
    }
    if (nbytes % sizeof(uint64_t) != 0) {
        gcraw_set_err(err_out,
                      "n00b_gcraw section size %zu is not a multiple of 8",
                      nbytes);
        return false;
    }

    const uint64_t *w      = (const uint64_t *)(const void *)bytes;
    size_t          nwords = nbytes / sizeof(uint64_t);
    size_t          pos    = 0;

    while (pos < nwords) {
        // COFF may align each emitted array within the section, leaving zero
        // words between otherwise self-delimiting records.
        if (w[pos] == 0) {
            pos++;
            continue;
        }
        size_t   start     = pos;
        uint64_t rec_words = w[start];
        if (rec_words < 6 || (uint64_t)(nwords - start) < rec_words) {
            gcraw_set_err(err_out,
                          "n00b_gcraw record at word %zu has bad length %llu",
                          start, (unsigned long long)rec_words);
            return false;
        }
        size_t end = start + (size_t)rec_words; // exclusive

        uint64_t kind      = w[start + 1];
        uint64_t type_hash = w[start + 2];
        uint64_t stride    = w[start + 3];
        uint64_t n_fixed   = w[start + 4];

        if (kind != NCC_GCRAW_KIND_TYPEMAP) {
            pos = end; // forward-compatible: skip unknown record kinds
            continue;
        }
        if (set_has(set, type_hash)) {
            pos = end; // first writer wins
            continue;
        }

        size_t p = start + 5;
        if ((uint64_t)(end - p) < n_fixed) {
            gcraw_set_err(err_out,
                          "n00b_gcraw record %llu: fixed-offset count %llu "
                          "overruns record",
                          (unsigned long long)type_hash,
                          (unsigned long long)n_fixed);
            return false;
        }
        size_t fixed_at = p;
        p += (size_t)n_fixed;

        if (p >= end) {
            gcraw_set_err(err_out,
                          "n00b_gcraw record %llu: missing variant count",
                          (unsigned long long)type_hash);
            return false;
        }
        uint64_t n_variants = w[p++];

        ncc_gcraw_record_t rec = {
            .type_hash     = type_hash,
            .stride        = stride,
            .n_fixed       = n_fixed,
            .fixed_offsets = dup_words(w, fixed_at, n_fixed),
            .n_variants    = n_variants,
            .variants      = n_variants
                               ? ncc_alloc_array(ncc_gcraw_variant_t,
                                                 (size_t)n_variants)
                               : nullptr,
        };
        if (rec.variants) {
            memset(rec.variants, 0,
                   (size_t)n_variants * sizeof(ncc_gcraw_variant_t));
        }

        bool bad = false;
        for (uint64_t v = 0; v < n_variants && !bad; v++) {
            if ((size_t)(end - p) < 2) {
                bad = true;
                break;
            }
            uint64_t sel_off = w[p++];
            uint64_t n_arms  = w[p++];
            rec.variants[v].selector_offset = sel_off;
            rec.variants[v].n_arms          = n_arms;
            rec.variants[v].arms = n_arms ? ncc_alloc_array(ncc_gcraw_arm_t,
                                                            (size_t)n_arms)
                                          : nullptr;
            if (rec.variants[v].arms) {
                memset(rec.variants[v].arms, 0,
                       (size_t)n_arms * sizeof(ncc_gcraw_arm_t));
            }
            for (uint64_t a = 0; a < n_arms && !bad; a++) {
                if ((size_t)(end - p) < 2) {
                    bad = true;
                    break;
                }
                uint64_t selector = w[p++];
                uint64_t n_off    = w[p++];
                if ((uint64_t)(end - p) < n_off) {
                    bad = true;
                    break;
                }
                rec.variants[v].arms[a].selector  = selector;
                rec.variants[v].arms[a].n_offsets = n_off;
                rec.variants[v].arms[a].offsets   = dup_words(w, p, n_off);
                p += (size_t)n_off;
            }
        }

        if (bad || p != end) {
            free_record(&rec);
            gcraw_set_err(err_out,
                          "n00b_gcraw record %llu is malformed "
                          "(length/layout mismatch)",
                          (unsigned long long)type_hash);
            return false;
        }

        set_append(set, rec);
        pos = end;
    }

    return true;
}

static int
cmp_by_hash(const void *a, const void *b)
{
    uint64_t ha = ((const ncc_gcraw_record_t *)a)->type_hash;
    uint64_t hb = ((const ncc_gcraw_record_t *)b)->type_hash;
    return (ha < hb) ? -1 : (ha > hb) ? 1 : 0;
}

char *
ncc_gcraw_generate_c(const ncc_gcraw_set_t *set)
{
    ncc_buffer_t *out = ncc_buffer_empty();
    ncc_buffer_puts(out,
                    "// Generated by ncc --ncc-gcmap-prelink. Do not edit.\n"
                    "#include \"core/codegen_abi.h\"\n");

    // Stable sorted view by type_hash for the table + a deterministic binary
    // search at runtime. Sub-arrays keep their per-record index for naming.
    ncc_gcraw_record_t *sorted = nullptr;
    if (set->len) {
        sorted = ncc_alloc_array(ncc_gcraw_record_t, set->len);
        memcpy(sorted, set->records, set->len * sizeof(ncc_gcraw_record_t));
        qsort(sorted, set->len, sizeof(ncc_gcraw_record_t), cmp_by_hash);
    }

    for (size_t i = 0; i < set->len; i++) {
        const ncc_gcraw_record_t *r = &sorted[i];

        if (r->n_fixed) {
            ncc_buffer_printf(out, "static const uint64_t __g_off_%zu[]={", i);
            for (uint64_t k = 0; k < r->n_fixed; k++) {
                ncc_buffer_printf(out, "%s%lluULL", k ? "," : "",
                                  (unsigned long long)r->fixed_offsets[k]);
            }
            ncc_buffer_puts(out, "};");
        }

        for (uint64_t v = 0; v < r->n_variants; v++) {
            const ncc_gcraw_variant_t *vv = &r->variants[v];
            for (uint64_t a = 0; a < vv->n_arms; a++) {
                ncc_buffer_printf(out,
                                  "static const uint64_t __g_voff_%zu_%llu_%llu[]={",
                                  i, (unsigned long long)v, (unsigned long long)a);
                for (uint64_t k = 0; k < vv->arms[a].n_offsets; k++) {
                    ncc_buffer_printf(out, "%s%lluULL", k ? "," : "",
                                      (unsigned long long)vv->arms[a].offsets[k]);
                }
                ncc_buffer_puts(out, "};");
            }
            ncc_buffer_printf(out,
                              "static const n00b_gc_variant_arm_t __g_arm_%zu_%llu[]={",
                              i, (unsigned long long)v);
            for (uint64_t a = 0; a < vv->n_arms; a++) {
                ncc_buffer_printf(out,
                                  "%s{.selector=%lluULL,.ptr_offset_count=%lluULL,"
                                  ".ptr_offsets=__g_voff_%zu_%llu_%llu}",
                                  a ? "," : "",
                                  (unsigned long long)vv->arms[a].selector,
                                  (unsigned long long)vv->arms[a].n_offsets,
                                  i, (unsigned long long)v, (unsigned long long)a);
            }
            ncc_buffer_puts(out, "};");
        }

        if (r->n_variants) {
            ncc_buffer_printf(out,
                              "static const n00b_gc_variant_field_t __g_var_%zu[]={",
                              i);
            for (uint64_t v = 0; v < r->n_variants; v++) {
                ncc_buffer_printf(out,
                                  "%s{.selector_offset=%lluULL,.arm_count=%lluULL,"
                                  ".arms=__g_arm_%zu_%llu}",
                                  v ? "," : "",
                                  (unsigned long long)r->variants[v].selector_offset,
                                  (unsigned long long)r->variants[v].n_arms,
                                  i, (unsigned long long)v);
            }
            ncc_buffer_puts(out, "};");
        }

        char offs_ref[48];
        char var_ref[48];
        if (r->n_fixed) {
            snprintf(offs_ref, sizeof(offs_ref), "__g_off_%zu", i);
        }
        else {
            offs_ref[0] = '0';
            offs_ref[1] = '\0';
        }
        if (r->n_variants) {
            snprintf(var_ref, sizeof(var_ref), "__g_var_%zu", i);
        }
        else {
            var_ref[0] = '0';
            var_ref[1] = '\0';
        }
        ncc_buffer_printf(out,
                          "static const n00b_gc_struct_layout_t __g_lay_%zu="
                          "{.stride=%lluULL,.count=1,.offset_count=%lluULL,"
                          ".offsets=%s,.variant_count=%lluULL,.variants=%s};",
                          i, (unsigned long long)r->stride,
                          (unsigned long long)r->n_fixed, offs_ref,
                          (unsigned long long)r->n_variants, var_ref);
    }

    // Emit the table referencing the per-record layouts.
    ncc_buffer_puts(out,
                    "const n00b_gc_type_map_entry_t n00b_gcmap_table[]={");
    for (size_t i = 0; i < set->len; i++) {
        ncc_buffer_printf(out, "%s{.type_hash=%lluULL,.layout=&__g_lay_%zu}",
                          i ? "," : "",
                          (unsigned long long)sorted[i].type_hash, i);
    }
    ncc_buffer_puts(out, "};");
    ncc_buffer_printf(out,
                      "const unsigned long n00b_gcmap_count=%zuULL;\n",
                      set->len);

    ncc_free(sorted);
    return ncc_buffer_take(out);
}

void
ncc_gcraw_set_free(ncc_gcraw_set_t *set)
{
    if (!set) {
        return;
    }
    for (size_t i = 0; i < set->len; i++) {
        free_record(&set->records[i]);
    }
    ncc_free(set->records);
    set->records = nullptr;
    set->len     = 0;
    set->cap     = 0;
}
