#include "parse/crt_entry.h"
#include "lib/alloc.h"
#include "util/platform.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
assert_contains(const char *haystack, const char *needle)
{
    if (!strstr(haystack, needle)) {
        fprintf(stderr, "missing generated entry fragment: %s\n", needle);
        abort();
    }
}

static void
assert_not_contains(const char *haystack, const char *needle)
{
    if (strstr(haystack, needle)) {
        fprintf(stderr, "unexpected generated entry fragment: %s\n", needle);
        abort();
    }
}

static void
assert_order(const char *haystack, const char *before, const char *after)
{
    const char *b = strstr(haystack, before);
    const char *a = strstr(haystack, after);

    if (!b || !a || b >= a) {
        fprintf(stderr, "generated entry order mismatch: %s before %s\n",
                before, after);
        abort();
    }
}

static void
test_link_mode_detection(void)
{
    ncc_opts_t opts = {0};

    assert(!ncc_is_link_invocation(nullptr));
    assert(!ncc_is_link_invocation(&opts));

    opts.input_file = "test.c";
    assert(ncc_is_link_invocation(&opts));

    opts.has_c = true;
    assert(!ncc_is_link_invocation(&opts));

    opts.has_c = false;
    opts.has_E = true;
    assert(!ncc_is_link_invocation(&opts));

    opts.has_E = false;
    opts.has_S = true;
    assert(!ncc_is_link_invocation(&opts));

    opts.has_S             = false;
    opts.has_fsyntax_only  = true;
    assert(!ncc_is_link_invocation(&opts));

    opts.has_fsyntax_only = false;
    opts.has_dep_only     = true;
    assert(!ncc_is_link_invocation(&opts));
}

static void
write_or_die(FILE *f, const char *text)
{
    if (fputs(text, f) == EOF) {
        perror("fputs");
        abort();
    }
}

static void
compile_generated_source(const char *source)
{
    const char *cc = getenv("NCC_CRT_TEST_CC");
    if (!cc || !*cc) {
        cc = "clang";
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_crt_entry_test_", &tmp_err)) {
        fprintf(stderr, "temporary workspace failed: %s\n",
                tmp_err ? tmp_err : "unknown error");
        ncc_free(tmp_err);
        abort();
    }

    const char *dir = ncc_temp_workspace_path(&tmp);
    char *src_path = ncc_temp_workspace_join(&tmp, "entry.c");
    char *header_path = ncc_temp_workspace_join(&tmp, "n00b_crt.h");
    char *obj_path = ncc_temp_workspace_join(&tmp, "entry.o");
    assert(src_path && header_path && obj_path);

    FILE *h = fopen(header_path, "w");
    if (!h) {
        perror("fopen header");
        abort();
    }

    write_or_die(h,
                 "#pragma once\n"
                 "[[noreturn]] void n00b_crt_main(int argc, char **argv, "
                 "char **envp);\n"
                 "void n00b_crt_run_init_array(void);\n");

    if (fclose(h) != 0) {
        perror("fclose header");
        abort();
    }

    FILE *f = fopen(src_path, "w");
    if (!f) {
        perror("fopen source");
        abort();
    }

    write_or_die(f,
                 "#include \"n00b_crt.h\"\n"
                 "void n00b_init_core_simple(int argc, char **argv);\n"
                 "void n00b_init_late(void);\n"
                 "[[noreturn]] void n00b_exit(int rc);\n");
    write_or_die(f, source);

    if (fclose(f) != 0) {
        perror("fclose");
        abort();
    }

    const char *args[] = {
        cc, "-std=gnu23", "-Wall", "-Wextra", "-Werror",
        "-I", dir, "-c", src_path, "-o", obj_path, nullptr,
    };
    ncc_process_spec_t spec = {
        .program = cc,
        .argv = args,
        .capture_stderr = true,
    };
    ncc_process_result_t result = {0};
    bool launched = ncc_process_run(&spec, &result);
    bool ok = launched && result.term_kind == NCC_PROCESS_TERM_EXITED
              && result.exit_code == 0;
    if (!ok) {
        fprintf(stderr, "generated CRT entry compile failed%s%s\n",
                result.stderr_data ? ": " : "",
                result.stderr_data ? result.stderr_data : "");
    }
    ncc_process_result_free(&result);
    ncc_free(src_path);
    ncc_free(header_path);
    ncc_free(obj_path);
    ncc_temp_workspace_cleanup(&tmp);

    if (!ok) {
        abort();
    }
}

static void
test_emit_shape_and_compile(void)
{
    ncc_opts_t opts = {0};

    assert(ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_BASE) == nullptr);

    opts.input_file = "test.c";

    const char *source = ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_BASE);
    assert(source);

    assert_contains(source, "# line 1 \"ncc-generated-crt-entry.c\"");
    assert_contains(source, "# line 1 \"ncc-generated-crt-entry-end.c\"");
    assert_contains(source, "n00b_crt_main");
    assert_contains(source, "n00b_crt_run_init_array();");
    assert_contains(source, "n00b_init_core_simple(argc, argv);");
    assert_contains(source, "n00b_init_late();");
    assert_contains(source, "extern int main(int argc, char **argv);");
    assert_contains(source,
                    "[[gnu::visibility(\"hidden\")]] void *__dso_handle");
    assert_not_contains(source, "__attribute__");
    assert_contains(source, "int rc = main(argc, argv);");
    assert_not_contains(source, "n00b_shutdown_simple();");
    assert_contains(source, "ncc_n00b_default_runtime_option_t");
    assert_contains(source,
                    "[[gnu::weak]] ncc_n00b_default_runtime_option_t n00b_default_runtime;");
    assert_contains(source, "__ncc_crt_exit_after_main(rc);");
    assert_order(source, "if (n00b_default_runtime.has_value)", "n00b_exit(rc);");
    assert_order(source, "n00b_exit(rc);", "exit(rc);");
    assert_contains(source, "SEAM (WP-003 / D-014)");
    assert_contains(source, "SEAM (WP-002 / D-002 / D-007)");
    compile_generated_source(source);

    const char *final_source = ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_FINAL);
    assert(final_source);
    assert_contains(final_source, "extern int main(int argc, char **argv);");
    assert_contains(final_source, "extern void *n00b_crt_apply_comptime_image(void);");
    assert_contains(final_source, "(void)n00b_crt_apply_comptime_image();");
    assert_contains(final_source, "int rc = main(argc, argv);");
    assert_contains(final_source, "__ncc_crt_exit_after_main(rc);");
    assert_not_contains(final_source, "comptime_main");

    const char *ct_source =
        ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_COMPTIME_RUN);
    assert(ct_source);
    assert_contains(ct_source, "# line 1 \"ncc-generated-crt-entry.c\"");
    assert_contains(ct_source, "# line 1 \"ncc-generated-crt-entry-end.c\"");
    assert_contains(ct_source, "n00b_crt_main");
    assert_contains(ct_source, "n00b_crt_run_init_array();");
    assert_contains(ct_source, "n00b_init_core_simple(argc, argv);");
    assert_contains(ct_source, "n00b_init_late();");
    assert_contains(ct_source,
                    "extern int comptime_main(int argc, char **argv, char **envp);");
    assert_contains(ct_source,
                    "[[gnu::visibility(\"hidden\")]] void *__dso_handle");
    assert_not_contains(ct_source, "__attribute__");
    assert_contains(ct_source, "int crc = 0;");
    assert_contains(ct_source, "crc = comptime_main(argc, argv, envp);");
    assert_order(ct_source, "n00b_init_core_simple(argc, argv);",
                 "n00b_init_late();");
    assert_order(ct_source, "n00b_init_late();",
                 "crc = comptime_main(argc, argv, envp);");
    assert_contains(ct_source, "n00b_exit(crc);");
    assert_contains(ct_source, "SEAM (WP-003)");
    assert_contains(ct_source, "SEAM (WP-004 / D-010 / D-020)");
    assert_order(ct_source, "crc = comptime_main(argc, argv, envp);",
                 "SEAM (WP-003)");
    assert_order(ct_source, "SEAM (WP-003)", "n00b_exit(crc);");
    assert_not_contains(ct_source, "extern int main");
    assert_not_contains(ct_source, "main(argc, argv)");
    assert_not_contains(ct_source, "n00b_shutdown_simple();");
    compile_generated_source(ct_source);

    const char *degrade_source =
        ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_DEGRADE);
    assert(degrade_source);
    assert_contains(degrade_source, "# line 1 \"ncc-generated-crt-entry.c\"");
    assert_contains(degrade_source, "# line 1 \"ncc-generated-crt-entry-end.c\"");
    assert_contains(degrade_source, "n00b_crt_main");
    assert_contains(degrade_source, "n00b_crt_run_init_array();");
    assert_contains(degrade_source, "n00b_init_core_simple(argc, argv);");
    assert_contains(degrade_source, "n00b_init_late();");
    assert_contains(degrade_source,
                    "extern int comptime_main(int argc, char **argv, char **envp);");
    assert_contains(degrade_source, "extern int main(int argc, char **argv);");
    assert_contains(degrade_source,
                    "[[gnu::visibility(\"hidden\")]] void *__dso_handle");
    assert_contains(degrade_source,
                    "(void)comptime_main(argc, argv, envp);");
    assert_contains(degrade_source, "int rc = main(argc, argv);");
    assert_order(degrade_source, "n00b_init_core_simple(argc, argv);",
                 "n00b_init_late();");
    assert_order(degrade_source, "n00b_init_late();",
                 "(void)comptime_main(argc, argv, envp);");
    assert_order(degrade_source, "(void)comptime_main(argc, argv, envp);",
                 "int rc = main(argc, argv);");
    assert_contains(degrade_source, "__ncc_crt_exit_after_main(rc);");
    assert_not_contains(degrade_source, "n00b_shutdown_simple();");
    assert_not_contains(degrade_source, "n00b_crt_apply_comptime_image");
    assert_not_contains(degrade_source, "__ncc_ct_roots");
    compile_generated_source(degrade_source);

    ncc_ct_var_t var = {
        .name     = NCC_STRING_STATIC("answer"),
        .typehash = 1,
        .linkage  = 1,
        .flags    = NCC_CT_VAR_FLAG_POINTER_ROOT,
    };
    ncc_ct_aggregate_t meta = {
        .has_comptime_main = true,
        .vars = &var,
        .n_vars = 1,
    };
    char odd_path[] = "/tmp/ncc\001A.bin";
    ncc_crt_entry_context_t entry_ctx = {
        .meta = &meta,
        .captured_image_path = odd_path,
    };

    const char *ct_capture_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_COMPTIME_RUN, &entry_ctx);
    assert(ct_capture_source);
    assert_contains(ct_capture_source,
                    "extern int n00b_crt_capture_static_roots");
    assert_contains(ct_capture_source, "extern void *answer;");
    assert_contains(ct_capture_source,
                    "n00b_crt_static_root_desc_t __ncc_ct_roots[]");
    assert_contains(ct_capture_source, ".addr=answer");
    assert_contains(ct_capture_source,
                    "n00b_crt_capture_static_roots(");
    assert_contains(ct_capture_source, "\"/tmp/ncc\\001A.bin\"");
    assert_not_contains(ct_capture_source, "\\x01");
    assert_order(ct_capture_source, "crc = comptime_main(argc, argv, envp);",
                 "int __ncc_ct_capture_rc = n00b_crt_capture_static_roots(");
    compile_generated_source(ct_capture_source);

    ncc_ct_static_init_t si = {
        .name       = NCC_STRING_STATIC("state"),
        .typehash   = 1,
        .kind       = NCC_CT_STATIC_INIT_CONST_RO,
        .flags      = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 0,
    };
    ncc_ct_aggregate_t static_only_meta = {
        .static_inits = &si,
        .n_static_inits = 1,
    };
    const char *static_snapshot_paths[] = { "/tmp/ncc-static-before.bin" };
    const char *static_check_paths[] = { "/tmp/ncc-static-after.bin" };
    ncc_crt_entry_context_t static_only_ctx = {
        .meta = &static_only_meta,
        .captured_image_path = "/tmp/ncc-static-final.bin",
        .static_init_snapshot_paths = static_snapshot_paths,
        .static_init_check_paths = static_check_paths,
    };
    const char *ct_static_only_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_COMPTIME_RUN, &static_only_ctx);
    assert(ct_static_only_source);
    assert_not_contains(ct_static_only_source, "extern int comptime_main");
    assert_not_contains(ct_static_only_source, "comptime_main(argc, argv, envp)");
    assert_contains(ct_static_only_source,
                    "extern int __ncc_static_init_prepare_state(void);");
    assert_not_contains(ct_static_only_source,
                        "__ncc_static_init_check_state");
    assert_contains(ct_static_only_source, "(void)envp;");
    assert_contains(ct_static_only_source, "int crc = 0;");
    assert_contains(ct_static_only_source,
                    "crc = __ncc_static_init_prepare_state();");
    assert_contains(ct_static_only_source,
                    "\"/tmp/ncc-static-before.bin\"");
    assert_contains(ct_static_only_source,
                    "\"/tmp/ncc-static-after.bin\"");
    assert_order(ct_static_only_source,
                 "crc = __ncc_static_init_prepare_state();",
                 "n00b_init_late();");
    assert_order(ct_static_only_source,
                 "n00b_init_late();",
                 "\"/tmp/ncc-static-before.bin\"");
    assert_order(ct_static_only_source,
                 "\"/tmp/ncc-static-before.bin\"",
                 "(void)envp;");
    assert_order(ct_static_only_source,
                 "(void)envp;",
                 "\"/tmp/ncc-static-after.bin\"");
    assert_order(ct_static_only_source,
                 "\"/tmp/ncc-static-after.bin\"",
                 "\"/tmp/ncc-static-final.bin\"");
    compile_generated_source(ct_static_only_source);

    ncc_ct_static_init_t writable_si = {
        .name       = NCC_STRING_STATIC("wstate"),
        .typehash   = 2,
        .kind       = NCC_CT_STATIC_INIT_WRITABLE,
        .flags      = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 0,
    };
    ncc_ct_static_init_t mixed_static_inits[] = { si, writable_si };
    ncc_ct_aggregate_t mixed_static_meta = {
        .static_inits = mixed_static_inits,
        .n_static_inits = 2,
    };
    const char *mixed_snapshot_paths[] = {
        "/tmp/ncc-mixed-state-before.bin",
        "/tmp/ncc-mixed-wstate-before.bin",
    };
    const char *mixed_check_paths[] = {
        "/tmp/ncc-mixed-state-after.bin",
        "/tmp/ncc-mixed-wstate-after.bin",
    };
    ncc_crt_entry_context_t mixed_static_ctx = {
        .meta = &mixed_static_meta,
        .captured_image_path = "/tmp/ncc-mixed-ro.bin",
        .captured_writable_image_path = "/tmp/ncc-mixed-writable.bin",
        .static_init_snapshot_paths = mixed_snapshot_paths,
        .static_init_check_paths = mixed_check_paths,
    };
    const char *ct_mixed_static_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_COMPTIME_RUN, &mixed_static_ctx);
    assert(ct_mixed_static_source);
    assert_contains(ct_mixed_static_source,
                    "extern int n00b_crt_capture_writable_static_roots");
    assert_contains(ct_mixed_static_source,
                    "n00b_crt_static_root_desc_t __ncc_ct_roots[]");
    assert_contains(ct_mixed_static_source,
                    "n00b_crt_static_root_desc_t __ncc_ct_writable_roots[]");
    assert_contains(ct_mixed_static_source, ".addr=state");
    assert_contains(ct_mixed_static_source, ".addr=wstate");
    assert_contains(ct_mixed_static_source, "\"/tmp/ncc-mixed-ro.bin\"");
    assert_contains(ct_mixed_static_source,
                    "\"/tmp/ncc-mixed-writable.bin\"");
    assert_order(ct_mixed_static_source,
                 "n00b_crt_capture_static_roots(",
                 "n00b_crt_capture_writable_static_roots(");
    compile_generated_source(ct_mixed_static_source);

    const char *final_capture_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_FINAL, &entry_ctx);
    assert(final_capture_source);
    assert_contains(final_capture_source, "extern void *answer;");
    assert_contains(final_capture_source,
                    "void **__ncc_ct_roots = n00b_crt_apply_comptime_image();");
    assert_contains(final_capture_source, "if (__ncc_ct_roots != nullptr)");
    assert_not_contains(final_capture_source, "(void *)0");
    assert_contains(final_capture_source, "answer = __ncc_ct_roots[0];");
    assert_order(final_capture_source, "n00b_init_core_simple(argc, argv);",
                 "n00b_crt_apply_comptime_image();");
    assert_order(final_capture_source, "answer = __ncc_ct_roots[0];",
                 "n00b_init_late();");
    assert_order(final_capture_source, "answer = __ncc_ct_roots[0];",
                 "int rc = main(argc, argv);");
    compile_generated_source(final_capture_source);

    const char *mixed_final_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_FINAL, &mixed_static_ctx);
    assert(mixed_final_source);
    assert_contains(mixed_final_source,
                    "extern void *n00b_crt_apply_comptime_image(void);");
    assert_contains(mixed_final_source,
                    "extern void *n00b_crt_apply_writable_image(void);");
    assert_contains(mixed_final_source,
                    "void **__ncc_ct_roots = n00b_crt_apply_comptime_image();");
    assert_contains(mixed_final_source,
                    "void **__ncc_ct_writable_roots = n00b_crt_apply_writable_image();");
    assert_contains(mixed_final_source, "state = __ncc_ct_roots[0];");
    assert_contains(mixed_final_source,
                    "wstate = __ncc_ct_writable_roots[0];");
    assert_order(mixed_final_source,
                 "n00b_crt_apply_comptime_image();",
                 "n00b_crt_apply_writable_image();");
    compile_generated_source(mixed_final_source);

    const char *degrade_capture_source = ncc_crt_emit_entry_ex(
        &opts, NCC_CRT_VARIANT_DEGRADE, &entry_ctx);
    assert(degrade_capture_source);
    assert_contains(degrade_capture_source,
                    "(void)comptime_main(argc, argv, envp);");
    assert_not_contains(degrade_capture_source,
                        "n00b_crt_capture_static_roots");
    assert_not_contains(degrade_capture_source,
                        "n00b_crt_apply_comptime_image");
    assert_not_contains(degrade_capture_source, "extern void *answer;");
    assert_not_contains(degrade_capture_source, "__ncc_ct_roots");

    ncc_ct_var_t bad_var = var;
    bad_var.linkage = 0;
    meta.vars = &bad_var;
    assert(ncc_crt_emit_entry_ex(&opts, NCC_CRT_VARIANT_COMPTIME_RUN,
                                 &entry_ctx) == nullptr);
    assert(ncc_crt_emit_entry_ex(&opts, NCC_CRT_VARIANT_DEGRADE,
                                 &entry_ctx) != nullptr);

    opts.has_c = true;
    assert(ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_BASE) == nullptr);
    assert(ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_FINAL) == nullptr);
    assert(ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_COMPTIME_RUN) == nullptr);
    assert(ncc_crt_emit_entry(&opts, NCC_CRT_VARIANT_DEGRADE) == nullptr);

    opts.has_c = false;
}

int
main(void)
{
    test_link_mode_detection();
    test_emit_shape_and_compile();
    puts("PASS: crt entry emitter");
    return 0;
}
