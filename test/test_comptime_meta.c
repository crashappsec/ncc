#include "parse/comptime_meta.h"
#include "parse/gcmap_prelink.h"

#include "lib/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
assert_contains(const char *haystack, const char *needle)
{
    if (!strstr(haystack, needle)) {
        fprintf(stderr, "missing metadata fragment: %s\n", needle);
        abort();
    }
}

static void
assert_not_contains(const char *haystack, const char *needle)
{
    if (strstr(haystack, needle)) {
        fprintf(stderr, "unexpected metadata fragment: %s\n", needle);
        abort();
    }
}

static void
test_empty_emit(void)
{
    assert(ncc_ct_emit_section_decl(nullptr, nullptr, 0, nullptr, 0) == nullptr);
    assert(ncc_ct_emit_section_decl(nullptr, nullptr,
                                    NCC_CT_MAIN_FLAG_OPTIONAL, nullptr, 0)
           == nullptr);
}

static void
test_comptime_main_emit_shape(void)
{
    ncc_ct_sig_t sig = {
        .argc     = 3,
        .has_argv = true,
        .has_envp = true,
    };

    const char *src = ncc_ct_emit_section_decl(nullptr, &sig, 0, nullptr, 0);
    assert(src);

    assert_contains(src, "# line 1 \"ncc-generated-comptime-meta.c\"");
    assert_contains(src, "# line 1 \"ncc-generated-comptime-meta-end.c\"");
    assert_contains(src, "[[gnu::used]]");
    assert_contains(src, "[[gnu::retain]]");
    assert_contains(src, NCC_CT_SECTION_ELF);
    assert_contains(src, NCC_CT_SECTION_MACHO);
    assert_contains(src, NCC_CT_SECTION_PE);
    assert_contains(src, "static const unsigned char __n00b_ct_meta[]");

    // Magic "N0CT", version 4, COMPTIME_MAIN TLV kind/len, sig, flags=0.
    assert_contains(src, "0x4e,0x30,0x43,0x54,0x04,0x00,");
    assert_contains(src, "0x01,0x00,0x04,0x00,0x03,0x01,0x01,0x00,");
    assert_contains(src, "0x00,0x00,0x00,0x00,");
    assert_not_contains(src, "NCC_CT_REC_VAR");

    ncc_free((void *)src);
}

static void
test_comptime_main_optional_flag_emit_shape(void)
{
    ncc_ct_sig_t sig = {
        .argc     = 3,
        .has_argv = true,
        .has_envp = true,
    };

    const char *src = ncc_ct_emit_section_decl(
        nullptr, &sig, NCC_CT_MAIN_FLAG_OPTIONAL, nullptr, 0);
    assert(src);
    assert_contains(src, "0x01,0x00,0x04,0x00,0x03,0x01,0x01,0x01,");
    ncc_free((void *)src);

    assert(ncc_ct_emit_section_decl(nullptr, &sig, 0x80, nullptr, 0)
           == nullptr);
}

static void
test_var_record_shape(void)
{
    ncc_ct_sig_t sig = {
        .argc     = 3,
        .has_argv = true,
        .has_envp = true,
    };
    ncc_ct_var_t var = {
        .name     = NCC_STRING_STATIC("answer"),
        .typehash = 0x0102030405060708ULL,
        .linkage  = 1,
        .flags    = NCC_CT_VAR_FLAG_POINTER_ROOT,
    };

    const char *src = ncc_ct_emit_section_decl(nullptr, &sig, 0, &var, 1);
    assert(src);

    // VAR TLV kind, payload len 18, little-endian typehash, external pointer root.
    assert_contains(src, "0x02,0x00,0x12,0x00,");
    assert_contains(src, "0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,0x01,0x01,");
    // name_len = 6, then ASCII "answer".
    assert_contains(src, "0x06,0x00,0x61,0x6e,0x73,0x77,0x65,0x72,");

    ncc_free((void *)src);
}

static void
test_var_record_rejects_oversized_payload(void)
{
    size_t too_long = UINT16_MAX - (8 + 1 + 1 + 2) + 1;
    char  *name     = malloc(too_long);
    assert(name);
    memset(name, 'x', too_long);

    ncc_ct_var_t var = {
        .name = {
            .data       = name,
            .u8_bytes   = too_long,
            .codepoints = too_long,
            .styling    = nullptr,
        },
        .typehash = 1,
        .linkage  = 1,
    };

    assert(ncc_ct_emit_section_decl(nullptr, nullptr, 0, &var, 1) == nullptr);
    free(name);
}

static void
test_var_record_rejects_unknown_fields(void)
{
    ncc_ct_var_t var = {
        .name     = NCC_STRING_STATIC("answer"),
        .typehash = 1,
        .linkage  = 2,
    };

    assert(ncc_ct_emit_section_decl(nullptr, nullptr, 0, &var, 1) == nullptr);

    var.linkage = 1;
    var.flags = 0x80;
    assert(ncc_ct_emit_section_decl(nullptr, nullptr, 0, &var, 1) == nullptr);
}

static void
test_static_init_record_shape(void)
{
    ncc_ct_static_init_t si = {
        .name       = NCC_STRING_STATIC("state"),
        .typehash   = 0x1112131415161718ULL,
        .kind       = NCC_CT_STATIC_INIT_WRITABLE,
        .flags      = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 1,
    };

    const char *src = ncc_ct_emit_section_decl_ex(
        nullptr, nullptr, 0, nullptr, 0, &si, 1);
    assert(src);

    // STATIC_INIT TLV kind, payload len 18, typehash, kind, flags, degrade, name.
    assert_contains(src, "0x03,0x00,0x12,0x00,");
    assert_contains(src, "0x18,0x17,0x16,0x15,0x14,0x13,0x12,0x11,0x01,0x01,0x01,");
    assert_contains(src, "0x05,0x00,0x73,0x74,0x61,0x74,0x65,");

    ncc_free((void *)src);
}

static void
test_static_init_record_rejects_unknown_fields(void)
{
    ncc_ct_static_init_t si = {
        .name     = NCC_STRING_STATIC("state"),
        .typehash = 1,
        .kind     = 2,
    };

    assert(ncc_ct_emit_section_decl_ex(nullptr, nullptr, 0, nullptr, 0,
                                       &si, 1) == nullptr);

    si.kind = NCC_CT_STATIC_INIT_CONST_RO;
    si.degrade_ok = 2;
    assert(ncc_ct_emit_section_decl_ex(nullptr, nullptr, 0, nullptr, 0,
                                       &si, 1) == nullptr);

    si.degrade_ok = 0;
    si.flags = 0x80;
    assert(ncc_ct_emit_section_decl_ex(nullptr, nullptr, 0, nullptr, 0,
                                       &si, 1) == nullptr);
}

static void
test_aggregate_static_init(void)
{
    ncc_ct_rec_t recs[2] = {
        {
            .kind = NCC_CT_REC_STATIC_INIT,
            .static_init = {
                .name = NCC_STRING_STATIC("state"),
                .typehash = 0x1234,
                .kind = NCC_CT_STATIC_INIT_CONST_RO,
                .flags = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
                .degrade_ok = 0,
            },
        },
        {
            .kind = NCC_CT_REC_STATIC_INIT,
            .static_init = {
                .name = NCC_STRING_STATIC("state"),
                .typehash = 0x1234,
                .kind = NCC_CT_STATIC_INIT_CONST_RO,
                .flags = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
                .degrade_ok = 0,
            },
        },
    };
    ncc_ct_rec_list_t list = {
        .records = recs,
        .n_records = 2,
        .n_objects_scanned = 2,
    };
    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;

    assert(ncc_ct_aggregate(&list, &agg, &err));
    assert(err == nullptr);
    assert(agg.n_static_inits == 1);
    assert(agg.static_inits[0].typehash == 0x1234);
    assert(agg.static_inits[0].kind == NCC_CT_STATIC_INIT_CONST_RO);
    assert(agg.static_inits[0].flags == NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT);
    assert(agg.static_inits[0].degrade_ok == 0);
    assert(agg.static_inits[0].name.u8_bytes == 5);
    assert(memcmp(agg.static_inits[0].name.data, "state", 5) == 0);
    ncc_ct_aggregate_free(&agg);
}

static void
test_aggregate_rejects_conflicting_main_flags(void)
{
    ncc_ct_rec_t recs[2] = {
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig = { .argc = 3, .has_argv = true, .has_envp = true },
            .main_flags = 0,
        },
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig = { .argc = 3, .has_argv = true, .has_envp = true },
            .main_flags = NCC_CT_MAIN_FLAG_OPTIONAL,
        },
    };
    ncc_ct_rec_list_t list = {
        .records = recs,
        .n_records = 2,
        .n_objects_scanned = 2,
    };
    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;

    assert(!ncc_ct_aggregate(&list, &agg, &err));
    assert(err);
    ncc_free(err);
}

static void
test_gcraw_alignment_padding(void)
{
    const uint64_t words[] = {
        0,
        6, NCC_GCRAW_KIND_TYPEMAP, 0x1234, 2, 0, 0,
        0, 0,
        7, NCC_GCRAW_KIND_TYPEMAP, 0x5678, 3, 1, 2, 0,
        0,
    };
    ncc_gcraw_set_t set = {0};
    char *err = nullptr;

    assert(ncc_gcraw_parse((const uint8_t *)words, sizeof(words), &set, &err));
    assert(err == nullptr);
    assert(set.len == 2);
    assert(set.records[0].type_hash == 0x1234);
    assert(set.records[1].type_hash == 0x5678);
    ncc_gcraw_set_free(&set);
}

int
main(void)
{
    test_empty_emit();
    test_comptime_main_emit_shape();
    test_comptime_main_optional_flag_emit_shape();
    test_var_record_shape();
    test_var_record_rejects_oversized_payload();
    test_var_record_rejects_unknown_fields();
    test_static_init_record_shape();
    test_static_init_record_rejects_unknown_fields();
    test_aggregate_static_init();
    test_aggregate_rejects_conflicting_main_flags();
    test_gcraw_alignment_padding();
    puts("PASS: comptime metadata emitter");
    return 0;
}
