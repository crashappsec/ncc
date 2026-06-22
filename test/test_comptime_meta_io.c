#include "parse/comptime_meta.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"
#include "util/type_normalize.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
die_process(const char *what, const ncc_process_result_t *proc)
{
    fprintf(stderr, "%s failed", what);
    if (proc && proc->stderr_data && proc->stderr_len) {
        fprintf(stderr, ": %.*s", (int)proc->stderr_len, proc->stderr_data);
    }
    fputc('\n', stderr);
    abort();
}

static void
compile_object(const char *ncc, const char *src, const char *out,
               bool no_ncc, int n_flags, const char *const *flags)
{
    int max_args = 1 + (no_ncc ? 1 : 0) + n_flags + 4 + 1;
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args);
    int argc = 0;

    argv[argc++] = ncc;
    if (no_ncc) {
        argv[argc++] = "--no-ncc";
    }
    for (int i = 0; i < n_flags; i++) {
        argv[argc++] = flags[i];
    }
    argv[argc++] = "-c";
    argv[argc++] = src;
    argv[argc++] = "-o";
    argv[argc++] = out;
    argv[argc] = nullptr;

    ncc_process_spec_t spec = {
        .program        = ncc,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok = launched && proc.exit_code == 0;

    ncc_free(argv);

    if (!ok) {
        die_process("object compile", &proc);
    }

    ncc_process_result_free(&proc);
}

static bool
tool_works(const char *program)
{
    if (!program || !program[0]) {
        return false;
    }

    const char *argv[] = { program, "--version", nullptr };
    ncc_process_spec_t spec = {
        .program        = program,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok = launched && proc.exit_code == 0;

    ncc_process_result_free(&proc);
    return ok;
}

static const char *
find_ar(void)
{
    const char *env = getenv("NCC_LLVM_AR");

    if (tool_works(env)) {
        return env;
    }
    if (tool_works("llvm-ar")) {
        return "llvm-ar";
    }
    if (tool_works("ar")) {
        return "ar";
    }

    return nullptr;
}

static void
create_archive(const char *archive_path, const char *obj_a, const char *obj_b)
{
    const char *ar = find_ar();
    assert(ar);

    const char *argv[] = {
        ar,
        "rcs",
        archive_path,
        obj_a,
        obj_b,
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = ar,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok = launched && proc.exit_code == 0;

    if (!ok) {
        die_process("archive create", &proc);
    }

    ncc_process_result_free(&proc);
}

static bool
has_main_record(const ncc_ct_rec_list_t *records)
{
    for (int i = 0; i < records->n_records; i++) {
        if (records->records[i].kind == NCC_CT_REC_COMPTIME_MAIN) {
            return true;
        }
    }
    return false;
}

static bool
has_answer_var(const ncc_ct_rec_list_t *records)
{
    for (int i = 0; i < records->n_records; i++) {
        const ncc_ct_rec_t *rec = &records->records[i];

        if (rec->kind != NCC_CT_REC_VAR) {
            continue;
        }
        if (rec->var.name.u8_bytes == 6
            && memcmp(rec->var.name.data, "answer", 6) == 0
            && rec->var.typehash == ncc_type_hash_u64("int")
            && rec->var.linkage == 1
            && rec->var.flags == 0) {
            return true;
        }
    }
    return false;
}

static bool
has_static_init_record(const ncc_ct_rec_list_t *records, const char *name,
                       uint64_t typehash, uint8_t kind, uint8_t flags,
                       uint8_t degrade_ok)
{
    size_t name_len = strlen(name);

    for (int i = 0; i < records->n_records; i++) {
        const ncc_ct_rec_t *rec = &records->records[i];

        if (rec->kind != NCC_CT_REC_STATIC_INIT) {
            continue;
        }
        if (rec->static_init.name.u8_bytes == name_len
            && memcmp(rec->static_init.name.data, name, name_len) == 0
            && rec->static_init.typehash == typehash
            && rec->static_init.kind == kind
            && rec->static_init.flags == flags
            && rec->static_init.degrade_ok == degrade_ok) {
            return true;
        }
    }
    return false;
}

static void
assert_read_ok(const char *path, ncc_ct_rec_list_t *records)
{
    char *err = nullptr;
    bool ok = ncc_ct_read_object(nullptr, path, records, &err);

    if (!ok) {
        fprintf(stderr, "metadata read failed for %s: %s\n", path,
                err ? err : "(no diagnostic)");
        abort();
    }
    ncc_free(err);
}

static void
write_static_init_metadata_source(const char *path)
{
    ncc_ct_static_init_t si = {
        .name       = NCC_STRING_STATIC("state"),
        .typehash   = ncc_type_hash_u64("ncc_string_t *"),
        .kind       = NCC_CT_STATIC_INIT_CONST_RO,
        .flags      = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 0,
    };
    const char *source = ncc_ct_emit_section_decl_ex(
        nullptr, nullptr, 0, nullptr, 0, &si, 1);
    assert(source);

    char *write_err = nullptr;
    assert(ncc_platform_write_file(path, source, strlen(source), &write_err));
    assert(write_err == nullptr);
    ncc_free((char *)source);
}

static void
write_plain_address_init_source(const char *path)
{
    static const char source[] =
        "static int ncc_state_backing = 7;\n"
        "int *state = &ncc_state_backing;\n"
        "int main(void) { return state != 0 ? 0 : 1; }\n";

    char *write_err = nullptr;
    assert(ncc_platform_write_file(path, source, strlen(source), &write_err));
    assert(write_err == nullptr);
}

static void
test_static_init_object_io(const char *static_init_obj,
                           const char *expected_type, const char *name,
                           uint8_t kind)
{
    uint64_t typehash = ncc_type_hash_u64(expected_type);
    size_t name_len = strlen(name);

    ncc_ct_rec_list_t records = {0};
    assert_read_ok(static_init_obj, &records);
    assert(records.n_objects_scanned == 1);
    if (!has_static_init_record(&records, name, typehash, kind,
                                NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT, 0)) {
        fprintf(stderr, "missing STATIC_INIT record in %s for %s kind=%u\n",
                static_init_obj, name, (unsigned)kind);
        for (int i = 0; i < records.n_records; i++) {
            const ncc_ct_rec_t *rec = &records.records[i];
            if (rec->kind == NCC_CT_REC_STATIC_INIT) {
                fprintf(stderr,
                        "  saw STATIC_INIT name=%.*s typehash=%llu kind=%u flags=%u degrade=%u\n",
                        (int)rec->static_init.name.u8_bytes,
                        rec->static_init.name.data,
                        (unsigned long long)rec->static_init.typehash,
                        (unsigned)rec->static_init.kind,
                        (unsigned)rec->static_init.flags,
                        (unsigned)rec->static_init.degrade_ok);
            }
            else {
                fprintf(stderr, "  saw record kind=%d\n", (int)rec->kind);
            }
        }
        abort();
    }

    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;
    assert(ncc_ct_aggregate(&records, &agg, &err));
    assert(err == nullptr);
    assert(agg.n_objects_scanned == 1);
    assert(agg.n_static_inits == 1);
    assert(agg.static_inits[0].typehash == typehash);
    assert(agg.static_inits[0].kind == kind);
    assert(agg.static_inits[0].flags
           == NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT);
    assert(agg.static_inits[0].degrade_ok == 0);
    assert(agg.static_inits[0].name.u8_bytes == name_len);
    assert(memcmp(agg.static_inits[0].name.data, name, name_len) == 0);
    ncc_ct_aggregate_free(&agg);
    ncc_ct_rec_list_free(&records);

    assert(ncc_ct_strip_section(nullptr, static_init_obj, &err));
    assert(err == nullptr);

    ncc_ct_rec_list_t stripped = {0};
    assert_read_ok(static_init_obj, &stripped);
    assert(stripped.n_records == 0);
    assert(stripped.n_objects_scanned == 1);
    ncc_ct_rec_list_free(&stripped);
}

static void
test_no_static_init_object_io(const char *obj)
{
    ncc_ct_rec_list_t records = {0};
    assert_read_ok(obj, &records);
    assert(records.n_objects_scanned == 1);

    for (int i = 0; i < records.n_records; i++) {
        assert(records.records[i].kind != NCC_CT_REC_STATIC_INIT);
    }

    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;
    assert(ncc_ct_aggregate(&records, &agg, &err));
    assert(err == nullptr);
    assert(agg.n_static_inits == 0);
    ncc_ct_aggregate_free(&agg);
    ncc_ct_rec_list_free(&records);
}

static void
test_read_aggregate_strip(const char *main_obj, const char *no_main_obj,
                          const char *var_obj)
{
    ncc_ct_rec_list_t absent = {0};
    assert_read_ok(no_main_obj, &absent);
    assert(absent.n_objects_scanned == 1);
    assert(absent.n_records == 0);
    ncc_ct_rec_list_free(&absent);

    ncc_ct_rec_list_t records = {0};
    assert_read_ok(main_obj, &records);
    assert(has_main_record(&records));
    assert_read_ok(var_obj, &records);
    assert(has_answer_var(&records));
    assert(records.n_objects_scanned == 2);

    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;
    assert(ncc_ct_aggregate(&records, &agg, &err));
    assert(err == nullptr);
    assert(agg.n_objects_scanned == 2);
    assert(agg.has_comptime_main);
    assert(agg.main_sig.argc == 3);
    assert(agg.main_sig.has_argv);
    assert(agg.main_sig.has_envp);
    assert(agg.main_flags == 0);
    assert(agg.n_vars == 1);
    assert(agg.vars[0].typehash == ncc_type_hash_u64("int"));
    assert(agg.vars[0].linkage == 1);
    assert(agg.vars[0].flags == 0);
    assert(agg.vars[0].name.u8_bytes == 6);
    assert(memcmp(agg.vars[0].name.data, "answer", 6) == 0);
    ncc_ct_aggregate_free(&agg);
    ncc_ct_rec_list_free(&records);

    assert(ncc_ct_strip_section(nullptr, var_obj, &err));
    assert(err == nullptr);

    ncc_ct_rec_list_t stripped = {0};
    assert_read_ok(var_obj, &stripped);
    assert(stripped.n_records == 0);
    assert(stripped.n_objects_scanned == 1);
    ncc_ct_rec_list_free(&stripped);
}

static void
test_archive_read(const char *archive_path)
{
    ncc_ct_rec_list_t records = {0};
    assert_read_ok(archive_path, &records);
    assert(records.n_objects_scanned == 2);
    assert(has_main_record(&records));
    assert(has_answer_var(&records));
    ncc_ct_rec_list_free(&records);
}

static void
test_archive_without_metadata_is_noop(const char *archive_path)
{
    ncc_ct_rec_list_t records = {0};
    assert_read_ok(archive_path, &records);
    assert(records.n_objects_scanned == 0);
    assert(records.n_records == 0);
    ncc_ct_rec_list_free(&records);
}

static void
test_no_magic_input_is_noop(const char *path)
{
    static const char bytes[] = "plain object without comptime metadata";
    char *write_err = nullptr;

    assert(ncc_platform_write_file(path, bytes, sizeof(bytes) - 1,
                                   &write_err));
    assert(write_err == nullptr);

    ncc_ct_rec_list_t records = {0};
    assert_read_ok(path, &records);
    assert(records.n_objects_scanned == 1);
    assert(records.n_records == 0);
    ncc_ct_rec_list_free(&records);
}

static void
test_archive_malformed_rolls_back(const char *archive_path)
{
    ncc_ct_rec_list_t records = {0};
    char *err = nullptr;

    assert(!ncc_ct_read_object(nullptr, archive_path, &records, &err));
    assert(err);
    assert(records.n_records == 0);
    assert(records.n_objects_scanned == 0);
    ncc_free(err);
    ncc_ct_rec_list_free(&records);
}

static void
test_conflicts(void)
{
    ncc_ct_rec_t main_recs[2] = {
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig  = { .argc = 1, .has_argv = true, .has_envp = false },
            .main_flags = 0,
        },
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig  = { .argc = 3, .has_argv = true, .has_envp = true },
            .main_flags = 0,
        },
    };
    ncc_ct_rec_list_t main_list = {
        .records = main_recs,
        .n_records = 2,
        .n_objects_scanned = 2,
    };
    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;
    assert(!ncc_ct_aggregate(&main_list, &agg, &err));
    assert(err);
    ncc_free(err);

    ncc_ct_rec_t flag_recs[2] = {
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig  = { .argc = 3, .has_argv = true, .has_envp = true },
            .main_flags = 0,
        },
        {
            .kind = NCC_CT_REC_COMPTIME_MAIN,
            .sig  = { .argc = 3, .has_argv = true, .has_envp = true },
            .main_flags = NCC_CT_MAIN_FLAG_OPTIONAL,
        },
    };
    ncc_ct_rec_list_t flag_list = {
        .records = flag_recs,
        .n_records = 2,
        .n_objects_scanned = 2,
    };
    err = nullptr;
    assert(!ncc_ct_aggregate(&flag_list, &agg, &err));
    assert(err);
    ncc_free(err);

    ncc_ct_rec_t var_recs[2] = {
        {
            .kind = NCC_CT_REC_VAR,
            .var = {
                .name = ncc_string_from_cstr("answer"),
                .typehash = 1,
                .linkage = 1,
            },
        },
        {
            .kind = NCC_CT_REC_VAR,
            .var = {
                .name = ncc_string_from_cstr("answer"),
                .typehash = 2,
                .linkage = 1,
            },
        },
    };
    ncc_ct_rec_list_t var_list = {
        .records = var_recs,
        .n_records = 2,
        .n_objects_scanned = 2,
    };
    err = nullptr;
    assert(!ncc_ct_aggregate(&var_list, &agg, &err));
    assert(err);
    ncc_free(err);
    ncc_free(var_recs[0].var.name.data);
    ncc_free(var_recs[1].var.name.data);
}

static void
test_malformed_section(const char *ncc, const char *bad_src,
                       const char *bad_obj, int n_flags,
                       const char *const *flags)
{
    static const char source[] =
        "#if defined(__APPLE__)\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_MACHO "\")]]\n"
        "#elif defined(_WIN32)\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_PE "\")]]\n"
        "#else\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_ELF "\")]]\n"
        "#endif\n"
        "static const unsigned char bad[] = { 0x4e, 0x30, 0x43, 0x54, 0x01 };\n"
        "int main(void) { return 0; }\n";

    char *write_err = nullptr;
    assert(ncc_platform_write_file(bad_src, source, strlen(source), &write_err));
    assert(write_err == nullptr);
    compile_object(ncc, bad_src, bad_obj, true, n_flags, flags);

    ncc_ct_rec_list_t records = {0};
    char *err = nullptr;
    assert(!ncc_ct_read_object(nullptr, bad_obj, &records, &err));
    assert(err);
    ncc_free(err);
    ncc_ct_rec_list_free(&records);
}

static void
write_raw_metadata_source(const char *path, const char *bytes)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    ncc_buffer_puts(buf,
        "#if defined(__APPLE__)\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_MACHO "\")]]\n"
        "#elif defined(_WIN32)\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_PE "\")]]\n"
        "#else\n"
        "[[gnu::used]] [[gnu::section(\"" NCC_CT_SECTION_ELF "\")]]\n"
        "#endif\n"
        "static const unsigned char bad[] = { ");
    ncc_buffer_puts(buf, bytes);
    ncc_buffer_puts(buf, " };\nint main(void) { return 0; }\n");

    char *source = ncc_buffer_take(buf);
    char *write_err = nullptr;
    assert(ncc_platform_write_file(path, source, strlen(source), &write_err));
    assert(write_err == nullptr);
    ncc_free(source);
}

static void
assert_metadata_read_fails(const char *obj_path)
{
    ncc_ct_rec_list_t records = {0};
    char *err = nullptr;

    assert(!ncc_ct_read_object(nullptr, obj_path, &records, &err));
    assert(err);
    ncc_free(err);
    ncc_ct_rec_list_free(&records);
}

static void
test_rejects_old_v1_var_record(const char *ncc, const char *bad_src,
                               const char *bad_obj, int n_flags,
                               const char *const *flags)
{
    write_raw_metadata_source(
        bad_src,
        "0x4e,0x30,0x43,0x54,0x01,0x00,"
        "0x02,0x00,0x11,0x00,"
        "0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,"
        "0x01,0x06,0x00,0x61,0x6e,0x73,0x77,0x65,0x72,"
        "0x00,0x00,0x00,0x00");
    compile_object(ncc, bad_src, bad_obj, true, n_flags, flags);
    assert_metadata_read_fails(bad_obj);
}

static void
test_rejects_invalid_var_fields(const char *ncc, const char *bad_src,
                                const char *bad_obj, int n_flags,
                                const char *const *flags)
{
    write_raw_metadata_source(
        bad_src,
        "0x4e,0x30,0x43,0x54,0x04,0x00,"
        "0x02,0x00,0x12,0x00,"
        "0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,"
        "0x02,0x00,0x06,0x00,0x61,0x6e,0x73,0x77,0x65,0x72,"
        "0x00,0x00,0x00,0x00");
    compile_object(ncc, bad_src, bad_obj, true, n_flags, flags);
    assert_metadata_read_fails(bad_obj);

    write_raw_metadata_source(
        bad_src,
        "0x4e,0x30,0x43,0x54,0x04,0x00,"
        "0x02,0x00,0x12,0x00,"
        "0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,"
        "0x01,0x80,0x06,0x00,0x61,0x6e,0x73,0x77,0x65,0x72,"
        "0x00,0x00,0x00,0x00");
    compile_object(ncc, bad_src, bad_obj, true, n_flags, flags);
    assert_metadata_read_fails(bad_obj);
}

static void
test_rejects_unknown_main_flags(const char *ncc, const char *bad_src,
                                const char *bad_obj, int n_flags,
                                const char *const *flags)
{
    write_raw_metadata_source(
        bad_src,
        "0x4e,0x30,0x43,0x54,0x04,0x00,"
        "0x01,0x00,0x04,0x00,0x03,0x01,0x01,0x80,"
        "0x00,0x00,0x00,0x00");
    compile_object(ncc, bad_src, bad_obj, true, n_flags, flags);
    assert_metadata_read_fails(bad_obj);
}

static void
test_aggregate_rejects_invalid_var_fields(void)
{
    ncc_ct_rec_t bad_rec = {
        .kind = NCC_CT_REC_VAR,
        .var = {
            .name = ncc_string_from_cstr("answer"),
            .typehash = 1,
            .linkage = 2,
        },
    };
    ncc_ct_rec_list_t list = {
        .records = &bad_rec,
        .n_records = 1,
    };
    ncc_ct_aggregate_t agg = {0};
    char *err = nullptr;

    assert(!ncc_ct_aggregate(&list, &agg, &err));
    assert(err);
    ncc_free(err);
    ncc_free(bad_rec.var.name.data);
}

int
main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <ncc> <main.c> <no-main.c> <var.c> [flags...]\n",
                argv[0]);
        return 2;
    }

    const char *ncc         = argv[1];
    const char *main_src    = argv[2];
    const char *no_main_src = argv[3];
    const char *var_src     = argv[4];
    const char *const *flags = argc > 5 ? (const char *const *)&argv[5]
                                        : nullptr;
    int         n_flags     = argc - 5;

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    assert(ncc_temp_workspace_create(&tmp, "ncc_ct_meta_io_", &tmp_err));
    assert(tmp_err == nullptr);

    char *main_obj    = ncc_temp_workspace_join(&tmp, "main.o");
    char *no_main_obj = ncc_temp_workspace_join(&tmp, "no-main.o");
    char *no_meta_obj = ncc_temp_workspace_join(&tmp, "no-meta.o");
    char *var_obj     = ncc_temp_workspace_join(&tmp, "var.o");
    char *bad_src     = ncc_temp_workspace_join(&tmp, "bad.c");
    char *bad_obj     = ncc_temp_workspace_join(&tmp, "bad.o");
    char *bad_v1_src  = ncc_temp_workspace_join(&tmp, "bad-v1.c");
    char *bad_v1_obj  = ncc_temp_workspace_join(&tmp, "bad-v1.o");
    char *bad_var_src = ncc_temp_workspace_join(&tmp, "bad-var.c");
    char *bad_var_obj = ncc_temp_workspace_join(&tmp, "bad-var.o");
    char *bad_flag_src = ncc_temp_workspace_join(&tmp, "bad-main-flag.c");
    char *bad_flag_obj = ncc_temp_workspace_join(&tmp, "bad-main-flag.o");
    char *static_init_src = ncc_temp_workspace_join(&tmp, "static-init.c");
    char *static_init_obj = ncc_temp_workspace_join(&tmp, "static-init.o");
    char *plain_address_src =
        ncc_temp_workspace_join(&tmp, "plain-address-init.c");
    char *plain_address_obj =
        ncc_temp_workspace_join(&tmp, "plain-address-init.o");
    char *archive     = ncc_temp_workspace_join(&tmp, "libct.a");
    char *no_meta_archive = ncc_temp_workspace_join(&tmp, "libct-empty.a");
    char *bad_archive = ncc_temp_workspace_join(&tmp, "libct-bad.a");
    char *no_magic_input = ncc_temp_workspace_join(&tmp, "no-magic.o");

    compile_object(ncc, main_src, main_obj, false, n_flags, flags);
    compile_object(ncc, no_main_src, no_main_obj, false, n_flags, flags);
    compile_object(ncc, no_main_src, no_meta_obj, false, n_flags, flags);
    compile_object(ncc, var_src, var_obj, false, n_flags, flags);
    create_archive(archive, main_obj, var_obj);
    create_archive(no_meta_archive, no_main_obj, no_meta_obj);
    write_static_init_metadata_source(static_init_src);
    compile_object(ncc, static_init_src, static_init_obj, true, n_flags, flags);
    write_plain_address_init_source(plain_address_src);
    compile_object(ncc, plain_address_src, plain_address_obj, false,
                   n_flags, flags);

    test_read_aggregate_strip(main_obj, no_main_obj, var_obj);
    test_static_init_object_io(static_init_obj, "ncc_string_t *", "state",
                               NCC_CT_STATIC_INIT_CONST_RO);
    test_no_static_init_object_io(plain_address_obj);
    test_archive_read(archive);
    test_archive_without_metadata_is_noop(no_meta_archive);
    test_no_magic_input_is_noop(no_magic_input);
    test_conflicts();
    test_malformed_section(ncc, bad_src, bad_obj, n_flags, flags);
    test_rejects_old_v1_var_record(ncc, bad_v1_src, bad_v1_obj,
                                   n_flags, flags);
    test_rejects_invalid_var_fields(ncc, bad_var_src, bad_var_obj,
                                    n_flags, flags);
    test_rejects_unknown_main_flags(ncc, bad_flag_src, bad_flag_obj,
                                    n_flags, flags);
    test_aggregate_rejects_invalid_var_fields();
    create_archive(bad_archive, main_obj, bad_obj);
    test_archive_malformed_rolls_back(bad_archive);

    ncc_free(main_obj);
    ncc_free(no_main_obj);
    ncc_free(no_meta_obj);
    ncc_free(var_obj);
    ncc_free(bad_src);
    ncc_free(bad_obj);
    ncc_free(bad_v1_src);
    ncc_free(bad_v1_obj);
    ncc_free(bad_var_src);
    ncc_free(bad_var_obj);
    ncc_free(bad_flag_src);
    ncc_free(bad_flag_obj);
    ncc_free(static_init_src);
    ncc_free(static_init_obj);
    ncc_free(plain_address_src);
    ncc_free(plain_address_obj);
    ncc_free(archive);
    ncc_free(no_meta_archive);
    ncc_free(bad_archive);
    ncc_free(no_magic_input);
    ncc_temp_workspace_cleanup(&tmp);

    puts("PASS: comptime metadata object IO");
    return 0;
}
