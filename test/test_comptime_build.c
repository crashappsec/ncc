#include "parse/comptime_build.h"
#include "parse/comptime_image_emit.h"
#include "parse/static_init_degrade.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
die_process(const char *what, const ncc_process_result_t *proc)
{
    fprintf(stderr, "%s failed", what);
    if (proc) {
        fprintf(stderr, " (exit=%d)", proc->exit_code);
    }
    if (proc && proc->stderr_data && proc->stderr_len) {
        fprintf(stderr, ": %.*s", (int)proc->stderr_len, proc->stderr_data);
    }
    fputc('\n', stderr);
    abort();
}

static void
run_checked(const char *what, const char *program, const char **argv)
{
    ncc_process_spec_t spec = {
        .program        = program,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok = launched && proc.exit_code == 0;

    if (!ok) {
        die_process(what, &proc);
    }

    ncc_process_result_free(&proc);
}

static void
compile_user_object(const char *ncc, const char *src, const char *out,
                    int n_flags, char **flags)
{
    int max_args = 1 + n_flags + 4 + 1;
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args);
    int argc = 0;

    argv[argc++] = ncc;
    for (int i = 0; i < n_flags; i++) {
        argv[argc++] = flags[i];
    }
    argv[argc++] = "-c";
    argv[argc++] = src;
    argv[argc++] = "-o";
    argv[argc++] = out;
    argv[argc] = nullptr;

    run_checked("user object compile", ncc, argv);
    ncc_free(argv);
}

static void
link_with_ncc(const char *ncc, const char *out, const char *const *inputs,
              int n_inputs, int n_flags, char **flags)
{
    int max_args = 1 + n_flags + 2 + n_inputs + 1;
    const char **argv = ncc_alloc_array(const char *, (size_t)max_args);
    int argc = 0;

    argv[argc++] = ncc;
    for (int i = 0; i < n_flags; i++) {
        argv[argc++] = flags[i];
    }
    argv[argc++] = "-o";
    argv[argc++] = out;
    for (int i = 0; i < n_inputs; i++) {
        argv[argc++] = inputs[i];
    }
    argv[argc] = nullptr;

    run_checked("ncc link", ncc, argv);
    ncc_free(argv);
}

static bool
program_works(const char *program)
{
    const char *argv[] = { program, "--version", nullptr };
    ncc_process_spec_t spec = {
        .program        = program,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool ok = ncc_process_run(&spec, &proc) && proc.exit_code == 0;
    ncc_process_result_free(&proc);
    return ok;
}

static const char *
find_objdump(void)
{
    const char *env = getenv("NCC_LLVM_OBJDUMP");
    if (env && *env) {
        return env;
    }
    if (program_works("llvm-objdump")) {
        return "llvm-objdump";
    }
    if (program_works("objdump")) {
        return "objdump";
    }
    fprintf(stderr, "llvm-objdump or objdump is required\n");
    abort();
}

static const char *
find_readelf(void)
{
    const char *env = getenv("NCC_LLVM_READELF");
    if (env && *env) {
        return env;
    }
    if (program_works("llvm-readelf")) {
        return "llvm-readelf";
    }
    if (program_works("readelf")) {
        return "readelf";
    }
    fprintf(stderr, "llvm-readelf or readelf is required\n");
    abort();
}

static char *
run_objdump(const char *path, const char *mode)
{
    const char *objdump = find_objdump();
    const char *argv[] = { objdump, mode, path, nullptr };
    ncc_process_spec_t spec = {
        .program        = objdump,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool ok = ncc_process_run(&spec, &proc) && proc.exit_code == 0;
    if (!ok) {
        die_process("objdump", &proc);
    }
    char *out = ncc_alloc_array(char, proc.stdout_len + 1);
    memcpy(out, proc.stdout_data, proc.stdout_len);
    ncc_process_result_free(&proc);
    return out;
}

static char *
run_readelf_sections(const char *path)
{
    const char *readelf = find_readelf();
    const char *argv[] = { readelf, "-SW", path, nullptr };
    ncc_process_spec_t spec = {
        .program        = readelf,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool ok = ncc_process_run(&spec, &proc) && proc.exit_code == 0;
    if (!ok) {
        die_process("readelf", &proc);
    }
    char *out = ncc_alloc_array(char, proc.stdout_len + 1);
    memcpy(out, proc.stdout_data, proc.stdout_len);
    ncc_process_result_free(&proc);
    return out;
}

static bool
text_has_image_section(const char *text)
{
    return strstr(text, NCC_CT_IMAGE_SECTION_ELF)
        || strstr(text, NCC_CT_IMAGE_SECTION_MACHO_SECT)
        || strstr(text, NCC_CT_IMAGE_SECTION_PE);
}

static bool
text_has_writable_image_section(const char *text)
{
    return strstr(text, NCC_CT_WRITABLE_IMAGE_SECTION_ELF)
        || strstr(text, NCC_CT_WRITABLE_IMAGE_SECTION_MACHO_SECT)
        || strstr(text, NCC_CT_WRITABLE_IMAGE_SECTION_PE);
}

static bool
text_has_static_init_degrade_section(const char *text)
{
    return strstr(text, NCC_STATIC_INIT_DEGRADE_SECTION_ELF)
        || strstr(text, NCC_STATIC_INIT_DEGRADE_SECTION_MACHO_SECT)
        || strstr(text, ".n00bsi");
}

static void
assert_image_section_present(const char *path)
{
    char *headers = run_objdump(path, "-h");
    if (!text_has_image_section(headers)) {
        fprintf(stderr, "image section missing from %s\n%s\n", path, headers);
        abort();
    }
    ncc_free(headers);
}

static void
assert_writable_image_section_present(const char *path)
{
    char *headers = run_objdump(path, "-h");
    if (!text_has_writable_image_section(headers)) {
        fprintf(stderr, "writable image section missing from %s\n%s\n",
                path, headers);
        abort();
    }
    ncc_free(headers);
}

static void
assert_static_init_degrade_section_present(const char *path)
{
    char *headers = run_objdump(path, "-h");
    if (!text_has_static_init_degrade_section(headers)) {
        fprintf(stderr, "static-init degrade section missing from %s\n%s\n",
                path, headers);
        abort();
    }
    ncc_free(headers);
}

static void
assert_object_contains_text(const char *path, const char *needle)
{
    char *contents = run_objdump(path, "-s");
    if (!strstr(contents, needle)) {
        fprintf(stderr, "object %s does not contain %s\n%s\n",
                path, needle, contents);
        abort();
    }
    ncc_free(contents);
}

static void
assert_object_symbol_present(const char *path, const char *needle)
{
    char *symbols = run_objdump(path, "-t");
    if (!strstr(symbols, needle)) {
        fprintf(stderr, "object %s missing symbol %s\n%s\n",
                path, needle, symbols);
        abort();
    }
    ncc_free(symbols);
}

static void
assert_linux_section_writable(const char *path, const char *section)
{
    char *sections = run_readelf_sections(path);
    char *line = strstr(sections, section);
    if (!line) {
        fprintf(stderr, "ELF section %s missing from %s\n%s\n",
                section, path, sections);
        abort();
    }

    char *line_end = strchr(line, '\n');
    if (line_end) {
        *line_end = '\0';
    }

    if (!strstr(line, " W") || !strstr(line, "A")) {
        fprintf(stderr, "ELF section is not writable+allocated in %s: %s\n",
                path, line);
        abort();
    }

    ncc_free(sections);
}

static void
compile_with_cc(const char *cc, const char *src, const char *out)
{
    const char *argv[] = {
        cc,
        "-std=gnu23",
        "-c",
        src,
        "-o",
        out,
        nullptr,
    };

    run_checked("cc compile", cc, argv);
}

static void
write_file_or_die(const char *path, const char *data)
{
    char *err = nullptr;
    if (!ncc_platform_write_file(path, data, strlen(data), &err)) {
        fprintf(stderr, "write failed for %s: %s\n", path,
                err ? err : "(no detail)");
        abort();
    }
    ncc_free(err);
}

static char *
read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long len = ftell(f);
    assert(len >= 0);
    assert(fseek(f, 0, SEEK_SET) == 0);
    char *data = ncc_alloc_array(char, (size_t)len + 1);
    assert(fread(data, 1, (size_t)len, f) == (size_t)len);
    assert(fclose(f) == 0);
    return data;
}

static bool
file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    return true;
}

static const char runtime_source_part1[] =
    "#include <stdint.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#if defined(__APPLE__)\n"
    "#include <mach-o/dyld.h>\n"
    "#include <mach-o/loader.h>\n"
    "#endif\n"
    "typedef int (*n00b_static_init_fn_t)(void);\n"
    "int ncc_ct_entry_seen = 0;\n"
    "[[gnu::weak]] void *ncc_ct_static_root_value = 0;\n"
    "void n00b_crt_run_init_array(void) {}\n"
    "static int n00b_run_degraded_static_init_range(\n"
    "    const n00b_static_init_fn_t *start,\n"
    "    const n00b_static_init_fn_t *end) {\n"
    "    if (!start || !end || end < start) return 0;\n"
    "    while (start < end) {\n"
    "        n00b_static_init_fn_t fn = *start++;\n"
    "        if (!fn) continue;\n"
    "        int rc = fn();\n"
    "        if (rc != 0) return rc;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "#if defined(__APPLE__)\n"
    "static int n00b_run_degraded_static_inits_macho_image(\n"
    "    const struct mach_header *hdr, intptr_t slide) {\n"
    "    if (!hdr || hdr->magic != MH_MAGIC_64) return 0;\n"
    "    const struct mach_header_64 *header = "
    "(const struct mach_header_64 *)hdr;\n"
    "    const uint8_t *cursor = (const uint8_t *)&header[1];\n"
    "    const uint8_t *const cmds_end = cursor + header->sizeofcmds;\n"
    "    for (uint32_t i = 0; i < header->ncmds; i++) {\n"
    "        if ((size_t)(cmds_end - cursor) < sizeof(struct load_command)) {\n"
    "            return 0;\n"
    "        }\n"
    "        const struct load_command *lc = (const struct load_command *)cursor;\n"
    "        if (lc->cmdsize < sizeof(struct load_command)\n"
    "            || lc->cmdsize > (uint32_t)(cmds_end - cursor)) {\n"
    "            return 0;\n"
    "        }\n"
    "        if (lc->cmd == LC_SEGMENT_64\n"
    "            && lc->cmdsize >= sizeof(struct segment_command_64)) {\n"
    "            const struct segment_command_64 *seg =\n"
    "                (const struct segment_command_64 *)cursor;\n"
    "            size_t sections_bytes =\n"
    "                (size_t)seg->nsects * sizeof(struct section_64);\n"
    "            if (sections_bytes <=\n"
    "                (size_t)(lc->cmdsize - sizeof(struct segment_command_64))) {\n"
    "                const struct section_64 *section =\n"
    "                    (const struct section_64 *)(seg + 1);\n"
    "                for (uint32_t j = 0; j < seg->nsects; j++) {\n"
    "                    if (strncmp(section[j].segname, \"__DATA\", 16) != 0\n"
    "                        || strncmp(section[j].sectname,\n"
    "                                   \"n00b_sinit\", 16) != 0) {\n"
    "                        continue;\n"
    "                    }\n"
    "                    uintptr_t start_addr =\n"
    "                        (uintptr_t)section[j].addr + (uintptr_t)slide;\n"
    "                    const n00b_static_init_fn_t *start =\n"
    "                        (const n00b_static_init_fn_t *)start_addr;\n"
    "                    const n00b_static_init_fn_t *end =\n"
    "                        start + (section[j].size / sizeof(*start));\n"
    "                    int rc = n00b_run_degraded_static_init_range(start, end);\n"
    "                    if (rc != 0) return rc;\n"
    "                }\n"
    "            }\n"
    "        }\n"
    "        cursor += lc->cmdsize;\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

static const char runtime_source_part2[] =
    "int n00b_run_degraded_static_inits(void) {\n"
    "    uint32_t image_count = _dyld_image_count();\n"
    "    for (uint32_t i = 0; i < image_count; i++) {\n"
    "        int rc = n00b_run_degraded_static_inits_macho_image(\n"
    "            _dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));\n"
    "        if (rc != 0) return rc;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "#elif defined(_WIN32)\n"
    "int n00b_run_degraded_static_inits(void) { return 0; }\n"
    "#else\n"
    "[[gnu::weak]] extern const n00b_static_init_fn_t __start_n00b_sinit[];\n"
    "[[gnu::weak]] extern const n00b_static_init_fn_t __stop_n00b_sinit[];\n"
    "int n00b_run_degraded_static_inits(void) {\n"
    "    if (!__start_n00b_sinit || !__stop_n00b_sinit) return 0;\n"
    "    return n00b_run_degraded_static_init_range(__start_n00b_sinit,\n"
    "                                              __stop_n00b_sinit);\n"
    "}\n"
    "#endif\n"
    "void n00b_init_core_simple(int argc, char **argv) {\n"
    "    (void)argc; (void)argv;\n"
    "    ncc_ct_entry_seen = 1;\n"
    "}\n"
    "void n00b_init_late(void) {}\n"
    "void n00b_init_simple(int argc, char **argv) {\n"
    "    n00b_init_core_simple(argc, argv);\n"
    "    int rc = n00b_run_degraded_static_inits();\n"
    "    if (rc != 0) _Exit(rc);\n"
    "    n00b_init_late();\n"
    "}\n"
    "void *n00b_crt_apply_comptime_image(void) {\n"
    "    static void *roots[1];\n"
    "    if (ncc_ct_static_root_value) {\n"
    "        roots[0] = ncc_ct_static_root_value;\n"
    "        return roots;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"
    "typedef struct {\n"
    "    unsigned int kind;\n"
    "    unsigned int scan_kind;\n"
    "    const void *addr;\n"
    "    unsigned long long size;\n"
    "    unsigned long long type_hash;\n"
    "    void *scan_cb;\n"
    "    void *scan_user;\n"
    "} n00b_crt_static_root_desc_t;\n"
    "int n00b_crt_capture_static_roots(const n00b_crt_static_root_desc_t *roots,\n"
    "                                  unsigned long long root_count,\n"
    "                                  const char *path) {\n"
    "    if (!roots || root_count != 1 || !roots[0].addr || !path) return 31;\n"
    "    FILE *f = fopen(path, \"wb\");\n"
    "    if (!f) return 32;\n"
    "    fputs(\"N00BCTIMG-STATIC-ROUTE\", f);\n"
    "    fclose(f);\n"
    "    return 0;\n"
    "}\n"
    "[[noreturn]] void n00b_exit(int rc) { _Exit(rc); }\n";

static const char start_source[] =
    "#if defined(__APPLE__) && defined(__aarch64__)\n"
    "    .text\n"
    "    .globl _n00b_start\n"
    "    .p2align 2\n"
    "_n00b_start:\n"
    "    mov x29, xzr\n"
    "    mov x30, xzr\n"
    "    bl _n00b_crt_main\n"
    "    brk #0\n"
    "#elif defined(__APPLE__) && defined(__x86_64__)\n"
    "    .text\n"
    "    .globl _n00b_start\n"
    "    .p2align 4, 0x90\n"
    "_n00b_start:\n"
    "    xorl %ebp, %ebp\n"
    "    andq $-16, %rsp\n"
    "    callq _n00b_crt_main\n"
    "    ud2\n"
    "#elif defined(__linux__) && defined(__aarch64__)\n"
    "    .text\n"
    "    .globl n00b_start\n"
    "    .type n00b_start,%function\n"
    "n00b_start:\n"
    "    mov x29, xzr\n"
    "    mov x30, xzr\n"
    "    ldr x0, [sp]\n"
    "    add x1, sp, #8\n"
    "    add x2, x1, x0, lsl #3\n"
    "    add x2, x2, #8\n"
    "    bl n00b_crt_main\n"
    "    brk #0\n"
    "    .size n00b_start, .-n00b_start\n"
    "    .section .note.GNU-stack,\"\",@progbits\n"
    "#elif defined(__linux__) && defined(__x86_64__)\n"
    "    .text\n"
    "    .globl n00b_start\n"
    "    .type n00b_start,@function\n"
    "n00b_start:\n"
    "    xorl %ebp, %ebp\n"
    "    movq (%rsp), %rdi\n"
    "    leaq 8(%rsp), %rsi\n"
    "    leaq 16(%rsp,%rdi,8), %rdx\n"
    "    andq $-16, %rsp\n"
    "    call n00b_crt_main\n"
    "    ud2\n"
    "    .size n00b_start, .-n00b_start\n"
    "    .section .note.GNU-stack,\"\",@progbits\n"
    "#else\n"
    "#error unsupported host for comptime build test stub\n"
    "#endif\n";

static const char good_source[] =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "[[n00b::comptime]] int answer = 42;\n"
    "int comptime_main(int argc, char **argv, char **envp) {\n"
    "    (void)envp;\n"
    "    const char *env = getenv(\"NCC_CT_PHASE4_ENV\");\n"
    "    FILE *f = fopen(argv[1], \"a\");\n"
    "    if (!f) return 3;\n"
    "    fprintf(f, \"argc=%d argv0=%s argv1=%s argv2=%s env=%s\\n\",\n"
    "            argc, argv[0], argv[1], argc > 2 ? argv[2] : \"\",\n"
    "            env ? env : \"\");\n"
    "    fclose(f);\n"
    "    return 0;\n"
    "}\n"
    "int main(int argc, char **argv) {\n"
    "    (void)argc; (void)argv;\n"
    "    return answer == 42 ? 0 : 5;\n"
    "}\n";

static const char fail_source[] =
    "int comptime_main(int argc, char **argv, char **envp) {\n"
    "    (void)argc; (void)argv; (void)envp;\n"
    "    return 7;\n"
    "}\n"
    "int main(void) { return 0; }\n";

static const char static_init_meta_source[] =
    "#if defined(__APPLE__)\n"
    "[[gnu::used]] [[gnu::retain]] "
    "[[gnu::section(\"__DATA,__n00b_ct\")]]\n"
    "#elif defined(_WIN32)\n"
    "[[gnu::used]] [[gnu::retain]] [[gnu::section(\".n00bct\")]]\n"
    "#else\n"
    "[[gnu::used]] [[gnu::retain]] [[gnu::section(\".n00b.comptime\")]]\n"
    "#endif\n"
    "static const unsigned char __n00b_ct_meta[] = {\n"
    "0x4e,0x30,0x43,0x54,0x04,0x00,\n"
    "0x03,0x00,0x12,0x00,\n"
    "0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,\n"
    "0x00,0x01,0x01,0x05,0x00,0x73,0x74,0x61,0x74,0x65,\n"
    "0x00,0x00,0x00,0x00,\n"
    "};\n";

static const char static_init_link_main_source[] =
    "extern int ncc_ct_entry_seen;\n"
    "static int static_payload = 17;\n"
    "static int prepare_count = 0;\n"
    "void *ncc_ct_static_root_value = &static_payload;\n"
    "const void *state = nullptr;\n"
    "int __ncc_static_init_prepare_state(void) {\n"
    "    prepare_count++;\n"
    "    state = &static_payload;\n"
    "    return 0;\n"
    "}\n"
    "typedef int (*__ncc_static_init_fn_state)(void);\n"
    "#if defined(__APPLE__)\n"
    "[[gnu::used]] [[gnu::retain]] [[gnu::section(\"__DATA,n00b_sinit\")]]\n"
    "#elif defined(_WIN32)\n"
    "[[gnu::used]] [[gnu::section(\".n00bsi$m\")]]\n"
    "#else\n"
    "[[gnu::used]] [[gnu::retain]] [[gnu::section(\"n00b_sinit\")]]\n"
    "#endif\n"
    "static __ncc_static_init_fn_state const "
    "__ncc_static_init_degrade_entry_state = "
    "__ncc_static_init_prepare_state;\n"
    "int main(void) {\n"
    "    if (ncc_ct_entry_seen != 1) return 9;\n"
    "    if (state != &static_payload) return 10;\n"
    "    if (prepare_count > 1) return 11;\n"
    "    return 0;\n"
    "}\n";

static void
assert_no_metadata_section(const char *path)
{
    ncc_ct_rec_list_t records = {0};
    char *err = nullptr;
    assert(ncc_ct_read_object(nullptr, path, &records, &err));
    assert(err == nullptr);
    assert(records.n_records == 0);
    ncc_ct_rec_list_free(&records);
}

static void
assert_static_init_only_metadata(const char *path)
{
    ncc_ct_rec_list_t records = {0};
    char *err = nullptr;
    assert(ncc_ct_read_object(nullptr, path, &records, &err));
    assert(err == nullptr);

    ncc_ct_aggregate_t agg = {0};
    assert(ncc_ct_aggregate(&records, &agg, &err));
    assert(err == nullptr);
    assert(!agg.has_comptime_main);
    assert(agg.n_vars == 0);
    assert(agg.n_static_inits == 1);
    assert(agg.static_inits[0].name.u8_bytes == 5);
    assert(memcmp(agg.static_inits[0].name.data, "state", 5) == 0);
    assert(agg.static_inits[0].typehash == 1);
    assert(agg.static_inits[0].kind == NCC_CT_STATIC_INIT_CONST_RO);
    assert(agg.static_inits[0].flags == NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT);
    assert(agg.static_inits[0].degrade_ok == 1);

    ncc_ct_aggregate_free(&agg);
    ncc_ct_rec_list_free(&records);
}

static void
test_collect_argv(void)
{
    const char *args[] = { "a", "b" };
    const char **argv = ncc_collect_comptime_argv("prog", args, 2);

    assert(argv);
    assert(strcmp(argv[0], "prog") == 0);
    assert(strcmp(argv[1], "a") == 0);
    assert(strcmp(argv[2], "b") == 0);
    assert(argv[3] == nullptr);
    ncc_free((void *)argv);
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <ncc> [ncc flags...]\n", argv[0]);
        return 2;
    }

    test_collect_argv();

    const char *ncc = argv[1];
    char **ncc_flags = argc > 2 ? &argv[2] : nullptr;
    int n_ncc_flags = argc - 2;
    const char *cc = getenv("NCC_CRT_TEST_CC");
    if (!cc || !*cc) {
        cc = "clang";
    }

    assert(setenv("NCC_CT_PHASE4_ENV", "present", 1) == 0);

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    assert(ncc_temp_workspace_create(&tmp, "ncc_ct_build_test_", &tmp_err));
    assert(tmp_err == nullptr);

    char *runtime_c = ncc_temp_workspace_join(&tmp, "runtime.c");
    char *runtime_o = ncc_temp_workspace_join(&tmp, "runtime.o");
    char *start_s = ncc_temp_workspace_join(&tmp, "start.S");
    char *start_o = ncc_temp_workspace_join(&tmp, "start.o");
    char *good_c = ncc_temp_workspace_join(&tmp, "good.c");
    char *good_o = ncc_temp_workspace_join(&tmp, "good.o");
    char *fail_c = ncc_temp_workspace_join(&tmp, "fail.c");
    char *fail_o = ncc_temp_workspace_join(&tmp, "fail.o");
    char *static_meta_c = ncc_temp_workspace_join(&tmp, "static-meta.c");
    char *static_meta_o = ncc_temp_workspace_join(&tmp, "static-meta.o");
    char *static_main_c = ncc_temp_workspace_join(&tmp, "static-main.c");
    char *static_main_o = ncc_temp_workspace_join(&tmp, "static-main.o");
    char *image_bytes = ncc_temp_workspace_join(&tmp, "image.bin");
    char *direct_image_o = ncc_temp_workspace_join(&tmp, "direct-image.o");
    char *direct_writable_image_o =
        ncc_temp_workspace_join(&tmp, "direct-writable-image.o");
    char *direct_static_init_degrade_o =
        ncc_temp_workspace_join(&tmp, "direct-static-init-degrade.o");
    char *linux_image_o = ncc_temp_workspace_join(&tmp, "linux-image.o");
    char *linux_writable_image_o =
        ncc_temp_workspace_join(&tmp, "linux-writable-image.o");
    char *sentinel = ncc_temp_workspace_join(&tmp, "sentinel.txt");
    char *output = ncc_temp_workspace_join(&tmp, "final");
    char *static_output = ncc_temp_workspace_join(&tmp, "static-final");
    char *fail_output = ncc_temp_workspace_join(&tmp, "final-fail");

    ncc_buffer_t *runtime_buf = ncc_buffer_empty();
    ncc_buffer_puts(runtime_buf, runtime_source_part1);
    ncc_buffer_puts(runtime_buf, runtime_source_part2);
    char *runtime_source = ncc_buffer_take(runtime_buf);
    write_file_or_die(runtime_c, runtime_source);
    ncc_free(runtime_source);
    write_file_or_die(start_s, start_source);
    write_file_or_die(good_c, good_source);
    write_file_or_die(fail_c, fail_source);
    write_file_or_die(static_meta_c, static_init_meta_source);
    write_file_or_die(static_main_c, static_init_link_main_source);
    write_file_or_die(image_bytes, "N00BCTIMG-PHASE2");

    compile_with_cc(cc, runtime_c, runtime_o);
    compile_with_cc(cc, start_s, start_o);
    compile_with_cc(cc, static_meta_c, static_meta_o);
    compile_with_cc(cc, static_main_c, static_main_o);
    assert_static_init_only_metadata(static_meta_o);
    compile_user_object(ncc, good_c, good_o, n_ncc_flags, ncc_flags);
    compile_user_object(ncc, fail_c, fail_o, n_ncc_flags, ncc_flags);

    ncc_opts_t opts = {
        .input_file = "<comptime-build-test>",
        .compiler   = cc,
    };

    char *err = nullptr;
    assert(ncc_emit_image_object(&opts, image_bytes, direct_image_o, &err));
    assert(err == nullptr);
    assert_image_section_present(direct_image_o);
    assert_object_contains_text(direct_image_o, "N00BCTIMG-PHASE2");
    assert_object_symbol_present(direct_image_o, "__n00b_ct_image");
    assert_object_symbol_present(direct_image_o, "__n00b_ct_image_len");
    assert_object_symbol_present(direct_image_o, "__n00b_ct_image_protect_len");
    assert(ncc_emit_writable_image_object(&opts, image_bytes,
                                          direct_writable_image_o, &err));
    assert(err == nullptr);
    assert_writable_image_section_present(direct_writable_image_o);
    assert_object_contains_text(direct_writable_image_o, "N00BCTIMG-PHASE2");
    assert_object_symbol_present(direct_writable_image_o,
                                 "__n00b_ct_writable_image");
    assert_object_symbol_present(direct_writable_image_o,
                                 "__n00b_ct_writable_image_len");

    ncc_ct_static_init_t degrade_si = {
        .name       = NCC_STRING_STATIC("state"),
        .typehash   = 1,
        .kind       = NCC_CT_STATIC_INIT_CONST_RO,
        .flags      = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 1,
    };
    ncc_ct_aggregate_t degrade_meta = {
        .static_inits   = &degrade_si,
        .n_static_inits = 1,
    };
    assert(ncc_emit_static_init_degrade_object(&opts, &degrade_meta,
                                               direct_static_init_degrade_o,
                                               &err));
    assert(err == nullptr);
    assert_static_init_degrade_section_present(direct_static_init_degrade_o);
    assert_object_symbol_present(direct_static_init_degrade_o,
                                 "__ncc_static_init_degrade_state");
    assert_object_symbol_present(direct_static_init_degrade_o,
                                 "__ncc_static_init_prepare_state");
    degrade_si.degrade_ok = 0;
    assert(!ncc_emit_static_init_degrade_object(&opts, &degrade_meta,
                                                direct_static_init_degrade_o,
                                                &err));
    assert(err != nullptr);
    assert(strstr(err,
                  "static initializer 'state' cannot be lowered to runtime "
                  "initialization for this target"));
    ncc_free(err);
    err = nullptr;

    const char *linux_target_args[] = { "-target", "x86_64-linux-gnu" };
    ncc_opts_t linux_opts = opts;
    linux_opts.clang_args = linux_target_args;
    linux_opts.n_clang_args = 2;
    assert(ncc_emit_image_object(&linux_opts, image_bytes, linux_image_o, &err));
    assert(err == nullptr);
    assert_linux_section_writable(linux_image_o, NCC_CT_IMAGE_SECTION_ELF);
    assert(ncc_emit_writable_image_object(&linux_opts, image_bytes,
                                          linux_writable_image_o, &err));
    assert(err == nullptr);
    assert_linux_section_writable(linux_writable_image_o,
                                  NCC_CT_WRITABLE_IMAGE_SECTION_ELF);

    const char *runtime_inputs[] = { start_o, runtime_o };
    const char *good_inputs[] = { good_o };
    const char *good_link_args[] = { good_o };
    const char *ct_argv[] = { "comptime-test", sentinel, "alpha", nullptr };
    ncc_comptime_plan_t plan = {
        .user_inputs    = good_inputs,
        .n_user_inputs  = 1,
        .output_file    = output,
        .runtime_inputs = runtime_inputs,
        .n_runtime_inputs = 2,
        .ordered_link_args = good_link_args,
        .n_ordered_link_args = 1,
        .comptime_argv  = ct_argv,
        .n_comptime_argv = 3,
        .captured_image_path = image_bytes,
    };

    int build_rc = ncc_comptime_run_and_link(&opts, &plan, &err);
    if (build_rc != 0) {
        fprintf(stderr, "initial comptime build failed: %s\n",
                err ? err : "(no diagnostic)");
    }
    assert(build_rc == 0);
    assert(err == nullptr);
    assert(file_exists(output));

    char *sentinel_text = read_text_file(sentinel);
    assert(strstr(sentinel_text, "argc=3"));
    assert(!strstr(sentinel_text, "argv0=comptime-test"));
    assert(strstr(sentinel_text, "argv2=alpha"));
    assert(strstr(sentinel_text, "env=present"));
    assert(strstr(sentinel_text, "\n") == strrchr(sentinel_text, '\n'));
    ncc_free(sentinel_text);

    const char *run_final_argv[] = { output, nullptr };
    run_checked("final binary", output, run_final_argv);
    assert_no_metadata_section(output);
    assert_image_section_present(output);

    const char *fail_inputs[] = { fail_o };
    const char *fail_link_args[] = { fail_o };
    ncc_comptime_plan_t fail_plan = plan;
    fail_plan.user_inputs = fail_inputs;
    fail_plan.ordered_link_args = fail_link_args;
    fail_plan.output_file = fail_output;
    write_file_or_die(fail_output, "PREEXISTING\n");
    err = nullptr;
    assert(ncc_comptime_run_and_link(&opts, &fail_plan, &err) != 0);
    assert(err && strstr(err, "comptime exited with status 7"));
    char *fail_output_text = read_text_file(fail_output);
    assert(strcmp(fail_output_text, "PREEXISTING\n") == 0);
    ncc_free(fail_output_text);
    ncc_free(err);

    const char *static_link_inputs[] = {
        static_main_o,
        static_meta_o,
        runtime_o,
        start_o,
    };
    link_with_ncc(ncc, static_output, static_link_inputs, 4,
                  n_ncc_flags, ncc_flags);
    const char *run_static_argv[] = { static_output, nullptr };
    run_checked("static-init-only final binary", static_output, run_static_argv);
    assert_no_metadata_section(static_output);
    assert_image_section_present(static_output);

    ncc_free(runtime_c);
    ncc_free(runtime_o);
    ncc_free(start_s);
    ncc_free(start_o);
    ncc_free(good_c);
    ncc_free(good_o);
    ncc_free(fail_c);
    ncc_free(fail_o);
    ncc_free(static_meta_c);
    ncc_free(static_meta_o);
    ncc_free(static_main_c);
    ncc_free(static_main_o);
    ncc_free(image_bytes);
    ncc_free(direct_image_o);
    ncc_free(direct_writable_image_o);
    ncc_free(direct_static_init_degrade_o);
    ncc_free(linux_image_o);
    ncc_free(linux_writable_image_o);
    ncc_free(sentinel);
    ncc_free(output);
    ncc_free(static_output);
    ncc_free(fail_output);
    ncc_temp_workspace_cleanup(&tmp);

    puts("PASS: comptime build driver");
    return 0;
}
