#include "parse/crt_entry.h"

#include "lib/alloc.h"
#include "lib/buffer.h"

#include <ctype.h>
#include <string.h>

static char *last_entry_source = nullptr;

bool
ncc_is_link_invocation(const ncc_opts_t *opts)
{
    return opts != nullptr && opts->input_file != nullptr
        && !opts->has_c && !opts->has_E && !opts->has_S
        && !opts->has_fsyntax_only && !opts->has_dep_only;
}

static bool
ct_var_is_pointer_root(const ncc_ct_var_t *var)
{
    return var != nullptr && (var->flags & NCC_CT_VAR_FLAG_POINTER_ROOT) != 0;
}

static bool
ct_static_init_is_pointer_root(const ncc_ct_static_init_t *si)
{
    return si != nullptr
        && (si->flags & NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT) != 0;
}

static bool
ct_static_init_is_value_root(const ncc_ct_static_init_t *si)
{
    return si != nullptr && !ct_static_init_is_pointer_root(si);
}

static bool
ct_static_init_is_writable(const ncc_ct_static_init_t *si)
{
    return si != nullptr && si->kind == NCC_CT_STATIC_INIT_WRITABLE;
}

static bool
entry_context_has_static_inits(const ncc_crt_entry_context_t *ctx)
{
    return ctx != nullptr && ctx->meta != nullptr
        && ctx->meta->n_static_inits > 0;
}

static int
entry_context_root_count(const ncc_crt_entry_context_t *ctx)
{
    int count = 0;

    if (ctx == nullptr || ctx->meta == nullptr) {
        return 0;
    }

    for (int i = 0; i < ctx->meta->n_vars; i++) {
        if (ct_var_is_pointer_root(&ctx->meta->vars[i])) {
            count++;
        }
    }
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        count++;
    }

    return count;
}

static int
entry_context_ro_root_count(const ncc_crt_entry_context_t *ctx)
{
    int count = 0;

    if (ctx == nullptr || ctx->meta == nullptr) {
        return 0;
    }

    for (int i = 0; i < ctx->meta->n_vars; i++) {
        if (ct_var_is_pointer_root(&ctx->meta->vars[i])) {
            count++;
        }
    }
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        if (!ct_static_init_is_writable(si)) {
            count++;
        }
    }

    return count;
}

static int
entry_context_writable_root_count(const ncc_crt_entry_context_t *ctx)
{
    int count = 0;

    if (ctx == nullptr || ctx->meta == nullptr) {
        return 0;
    }

    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        if (ct_static_init_is_writable(si)) {
            count++;
        }
    }

    return count;
}

static bool
entry_context_has_roots(const ncc_crt_entry_context_t *ctx)
{
    return entry_context_root_count(ctx) > 0;
}

static bool
entry_context_has_ro_roots(const ncc_crt_entry_context_t *ctx)
{
    return entry_context_ro_root_count(ctx) > 0;
}

static bool
entry_context_has_writable_roots(const ncc_crt_entry_context_t *ctx)
{
    return entry_context_writable_root_count(ctx) > 0;
}

static bool
entry_context_has_comptime_main(const ncc_crt_entry_context_t *ctx)
{
    return ctx == nullptr || ctx->meta == nullptr || ctx->meta->has_comptime_main;
}

static bool
ct_var_name_is_c_identifier(ncc_string_t name)
{
    if (!name.data || name.u8_bytes <= 0) {
        return false;
    }

    unsigned char first = (unsigned char)name.data[0];
    if (!(isalpha(first) || first == '_')) {
        return false;
    }

    for (size_t i = 1; i < name.u8_bytes; i++) {
        unsigned char ch = (unsigned char)name.data[i];
        if (!(isalnum(ch) || ch == '_')) {
            return false;
        }
    }

    return true;
}

static bool
ct_var_is_supported_root(const ncc_ct_var_t *var)
{
    return ct_var_is_pointer_root(var) && var->linkage == 1
        && ct_var_name_is_c_identifier(var->name);
}

static bool
ct_static_init_is_supported_root(const ncc_ct_static_init_t *si)
{
    return si != nullptr && ct_var_name_is_c_identifier(si->name);
}

static bool
entry_context_roots_supported(const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_roots(ctx)) {
        return true;
    }

    for (int i = 0; i < ctx->meta->n_vars; i++) {
        const ncc_ct_var_t *var = &ctx->meta->vars[i];
        if (ct_var_is_pointer_root(var) && !ct_var_is_supported_root(var)) {
            return false;
        }
    }
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        if (!ct_static_init_is_supported_root(si)) {
            return false;
        }
    }
    return true;
}

static void
emit_c_string_literal(ncc_buffer_t *buf, const char *s)
{
    ncc_buffer_putc(buf, '"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '\\':
            ncc_buffer_puts(buf, "\\\\");
            break;
        case '"':
            ncc_buffer_puts(buf, "\\\"");
            break;
        case '\n':
            ncc_buffer_puts(buf, "\\n");
            break;
        case '\r':
            ncc_buffer_puts(buf, "\\r");
            break;
        case '\t':
            ncc_buffer_puts(buf, "\\t");
            break;
        default:
            if (*p < 0x20 || *p >= 0x7f) {
                ncc_buffer_printf(buf, "\\%03o", (unsigned)*p);
            }
            else {
                ncc_buffer_putc(buf, (char)*p);
            }
            break;
        }
    }
    ncc_buffer_putc(buf, '"');
}

static void
emit_root_externs(ncc_buffer_t *buf, const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_roots(ctx)) {
        return;
    }

    for (int i = 0; i < ctx->meta->n_vars; i++) {
        const ncc_ct_var_t *var = &ctx->meta->vars[i];
        if (!ct_var_is_pointer_root(var)) {
            continue;
        }
        ncc_buffer_printf(buf, "extern void *%.*s;\n",
                          (int)var->name.u8_bytes, var->name.data);
    }
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        if (!ct_static_init_is_pointer_root(si)) {
            continue;
        }
        ncc_buffer_printf(buf, "extern void *%.*s;\n",
                          (int)si->name.u8_bytes, si->name.data);
    }
}

static bool
entry_context_has_static_value_roots(const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_static_inits(ctx)) {
        return false;
    }

    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        if (ct_static_init_is_value_root(&ctx->meta->static_inits[i])) {
            return true;
        }
    }
    return false;
}

static void
emit_static_root_desc_decl(ncc_buffer_t *buf)
{
    ncc_buffer_puts(buf,
        "typedef struct {\n"
        "    unsigned int kind;\n"
        "    unsigned int scan_kind;\n"
        "    const void *addr;\n"
        "    unsigned long long size;\n"
        "    unsigned long long type_hash;\n"
        "    void *scan_cb;\n"
        "    void *scan_user;\n"
        "} n00b_crt_static_root_desc_t;\n");
}

static void
emit_static_init_helper_externs(ncc_buffer_t *buf,
                                const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_static_inits(ctx)) {
        return;
    }

    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        ncc_buffer_printf(buf,
            "extern int __ncc_static_init_prepare_%.*s(void);\n",
            (int)si->name.u8_bytes, si->name.data);
        if (ct_static_init_is_value_root(si)) {
            ncc_buffer_printf(buf,
                "extern void *__ncc_static_init_addr_%.*s(void);\n"
                "extern void __ncc_static_init_apply_%.*s(void *);\n"
                "extern unsigned long long "
                "__ncc_static_init_size_%.*s(void);\n"
                "extern unsigned long long "
                "__ncc_static_init_typehash_%.*s(void);\n"
                "extern unsigned int "
                "__ncc_static_init_scan_kind_%.*s(void);\n"
                "extern void *__ncc_static_init_scan_cb_%.*s(void);\n"
                "extern void *__ncc_static_init_scan_user_%.*s(void);\n",
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data,
                (int)si->name.u8_bytes, si->name.data);
        }
    }
}

static void
emit_static_root_desc_for_var(ncc_buffer_t *buf, const ncc_ct_var_t *var)
{
    ncc_buffer_printf(buf,
        "            {.kind=1u,.scan_kind=0u,.addr=%.*s,"
        ".size=0ULL,.type_hash=0ULL,.scan_cb=0,.scan_user=0},\n",
        (int)var->name.u8_bytes, var->name.data);
}

static void
emit_static_root_desc_for_static_init(ncc_buffer_t *buf,
                                      const ncc_ct_static_init_t *si)
{
    if (ct_static_init_is_pointer_root(si)) {
        ncc_buffer_printf(buf,
            "            {.kind=1u,.scan_kind=0u,.addr=%.*s,"
            ".size=0ULL,.type_hash=0ULL,.scan_cb=0,.scan_user=0},\n",
            (int)si->name.u8_bytes, si->name.data);
        return;
    }

    ncc_buffer_printf(buf,
        "            {.kind=2u,"
        ".scan_kind=__ncc_static_init_scan_kind_%.*s(),"
        ".addr=__ncc_static_init_addr_%.*s(),"
        ".size=__ncc_static_init_size_%.*s(),"
        ".type_hash=__ncc_static_init_typehash_%.*s(),"
        ".scan_cb=__ncc_static_init_scan_cb_%.*s(),"
        ".scan_user=__ncc_static_init_scan_user_%.*s()},\n",
        (int)si->name.u8_bytes, si->name.data,
        (int)si->name.u8_bytes, si->name.data,
        (int)si->name.u8_bytes, si->name.data,
        (int)si->name.u8_bytes, si->name.data,
        (int)si->name.u8_bytes, si->name.data,
        (int)si->name.u8_bytes, si->name.data);
}

static void
emit_final_root_assignments(ncc_buffer_t *buf,
                            const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_roots(ctx)) {
        ncc_buffer_puts(buf, "    (void)n00b_crt_apply_comptime_image();\n");
        return;
    }

    if (entry_context_has_ro_roots(ctx)) {
        ncc_buffer_puts(buf,
            "    void **__ncc_ct_roots = n00b_crt_apply_comptime_image();\n"
            "    if (__ncc_ct_roots != nullptr) {\n");
        int root_ix = 0;
        for (int i = 0; i < ctx->meta->n_vars; i++) {
            const ncc_ct_var_t *var = &ctx->meta->vars[i];
            if (!ct_var_is_pointer_root(var)) {
                continue;
            }
            ncc_buffer_printf(buf, "        %.*s = __ncc_ct_roots[%d];\n",
                              (int)var->name.u8_bytes, var->name.data, root_ix++);
        }
        for (int i = 0; i < ctx->meta->n_static_inits; i++) {
            const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
            if (ct_static_init_is_writable(si)) {
                continue;
            }
            if (ct_static_init_is_pointer_root(si)) {
                ncc_buffer_printf(buf, "        %.*s = __ncc_ct_roots[%d];\n",
                                  (int)si->name.u8_bytes, si->name.data,
                                  root_ix++);
            }
            else {
                ncc_buffer_printf(buf,
                    "        __ncc_static_init_apply_%.*s("
                    "__ncc_ct_roots[%d]);\n",
                    (int)si->name.u8_bytes, si->name.data, root_ix++);
            }
        }
        ncc_buffer_puts(buf, "    }\n");
    }

    if (entry_context_has_writable_roots(ctx)) {
        ncc_buffer_puts(buf,
            "    void **__ncc_ct_writable_roots = n00b_crt_apply_writable_image();\n"
            "    if (__ncc_ct_writable_roots != nullptr) {\n");
        int root_ix = 0;
        for (int i = 0; i < ctx->meta->n_static_inits; i++) {
            const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
            if (!ct_static_init_is_writable(si)) {
                continue;
            }
            if (ct_static_init_is_pointer_root(si)) {
                ncc_buffer_printf(buf,
                    "        %.*s = __ncc_ct_writable_roots[%d];\n",
                    (int)si->name.u8_bytes, si->name.data, root_ix++);
            }
            else {
                ncc_buffer_printf(buf,
                    "        __ncc_static_init_apply_%.*s("
                    "__ncc_ct_writable_roots[%d]);\n",
                    (int)si->name.u8_bytes, si->name.data, root_ix++);
            }
        }
        ncc_buffer_puts(buf, "    }\n");
    }
}

static bool
emit_base_entry(ncc_buffer_t *buf, ncc_crt_variant_t variant,
                const ncc_crt_entry_context_t *ctx)
{
    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-crt-entry.c\"\n"
        "extern void n00b_crt_run_init_array(void);\n"
        "extern void n00b_init_core_simple(int argc, char **argv);\n"
        "extern void n00b_init_late(void);\n"
        "extern int n00b_run_degraded_static_inits(void);\n"
        "[[noreturn]] extern void n00b_exit(int rc);\n"
        "extern void exit(int rc);\n"
        "typedef struct n00b_runtime_t n00b_runtime_t;\n"
        "typedef struct {\n"
        "    _Bool has_value;\n"
        "    n00b_runtime_t *value;\n"
        "} ncc_n00b_default_runtime_option_t;\n"
        "[[gnu::weak]] ncc_n00b_default_runtime_option_t n00b_default_runtime;\n"
        "[[noreturn]] static void\n"
        "__ncc_crt_exit_after_main(int rc)\n"
        "{\n"
        "    if (n00b_default_runtime.has_value) {\n"
        "        n00b_exit(rc);\n"
        "    }\n"
        "    exit(rc);\n"
        "    __builtin_unreachable();\n"
        "}\n");
    if (variant == NCC_CRT_VARIANT_FINAL) {
        ncc_buffer_puts(buf, "extern void *n00b_crt_apply_comptime_image(void);\n");
        if (entry_context_has_writable_roots(ctx)) {
            ncc_buffer_puts(buf,
                "extern void *n00b_crt_apply_writable_image(void);\n");
        }
        if (entry_context_has_static_value_roots(ctx)) {
            emit_static_init_helper_externs(buf, ctx);
        }
        emit_root_externs(buf, ctx);
    }
    if (variant == NCC_CRT_VARIANT_DEGRADE) {
        ncc_buffer_puts(buf,
            "extern int comptime_main(int argc, char **argv, char **envp);\n");
    }
    ncc_buffer_puts(buf,
        "extern int main(int argc, char **argv);\n"
        "#if defined(__linux__)\n"
        "[[gnu::visibility(\"hidden\")]] void *__dso_handle = &__dso_handle;\n"
        "#endif\n"
        "\n"
        "/* PRE-RUNTIME CONTEXT: only raw C and declared runtime entry symbols\n"
        " * may be used before n00b_init_core_simple returns. No GC allocation,\n"
        " * n00b containers, conduits, or print APIs are available here. */\n"
        "[[noreturn]] void\n"
        "n00b_crt_main(int argc, char **argv, char **envp)\n"
        "{\n");
    if (variant != NCC_CRT_VARIANT_DEGRADE) {
        ncc_buffer_puts(buf, "    (void)envp;\n");
    }
    ncc_buffer_puts(buf,
        "    /* D-047: preserve libc crt0 ordering for constructor tables. */\n"
        "    n00b_crt_run_init_array();\n"
        "    n00b_init_core_simple(argc, argv);\n");
    if (variant != NCC_CRT_VARIANT_FINAL) {
        ncc_buffer_puts(buf,
            "    int __ncc_static_init_degrade_rc = n00b_run_degraded_static_inits();\n"
            "    if (__ncc_static_init_degrade_rc != 0) {\n"
            "        n00b_exit(__ncc_static_init_degrade_rc);\n"
            "    }\n");
    }
    ncc_buffer_puts(buf,
        "    /* SEAM (WP-003 / D-014): offset-relocatable RELRO image\n"
        "     * relocation prologue goes here, after n00b_init_core_simple and\n"
        "     * before comptime/main. */\n");
    if (variant == NCC_CRT_VARIANT_FINAL) {
        emit_final_root_assignments(buf, ctx);
    }
    ncc_buffer_puts(buf, "    n00b_init_late();\n");
    ncc_buffer_puts(buf,
        "    /* SEAM (WP-002 / D-002 / D-007): build-time driver sequencing\n"
        "     * goes here before user main. */\n"
    );
    if (variant == NCC_CRT_VARIANT_DEGRADE) {
        ncc_buffer_puts(buf,
            "    (void)comptime_main(argc, argv, envp);\n");
    }
    ncc_buffer_puts(buf,
        "    int rc = main(argc, argv);\n"
        "    __ncc_crt_exit_after_main(rc);\n"
        "}\n"
        "# line 1 \"ncc-generated-crt-entry-end.c\"\n");
    return true;
}

static void
emit_capture_epilogue(ncc_buffer_t *buf, const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_roots(ctx)) {
        return;
    }

    if (entry_context_has_ro_roots(ctx)) {
        ncc_buffer_puts(buf,
            "    if (crc == 0) {\n"
            "        n00b_crt_static_root_desc_t __ncc_ct_roots[] = {\n");
        for (int i = 0; i < ctx->meta->n_vars; i++) {
            const ncc_ct_var_t *var = &ctx->meta->vars[i];
            if (!ct_var_is_pointer_root(var)) {
                continue;
            }
            emit_static_root_desc_for_var(buf, var);
        }
        for (int i = 0; i < ctx->meta->n_static_inits; i++) {
            const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
            if (ct_static_init_is_writable(si)) {
                continue;
            }
            emit_static_root_desc_for_static_init(buf, si);
        }
        ncc_buffer_puts(buf,
            "        };\n"
            "        int __ncc_ct_capture_rc = n00b_crt_capture_static_roots(\n"
            "            __ncc_ct_roots, ");
        ncc_buffer_printf(buf, "%dULL, ", entry_context_ro_root_count(ctx));
        emit_c_string_literal(buf, ctx->captured_image_path);
        ncc_buffer_puts(buf,
            ");\n"
            "        if (__ncc_ct_capture_rc != 0) {\n"
            "            crc = __ncc_ct_capture_rc;\n"
            "        }\n"
            "    }\n");
    }

    if (entry_context_has_writable_roots(ctx)) {
        ncc_buffer_puts(buf,
            "    if (crc == 0) {\n"
            "        n00b_crt_static_root_desc_t __ncc_ct_writable_roots[] = {\n");
        for (int i = 0; i < ctx->meta->n_static_inits; i++) {
            const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
            if (!ct_static_init_is_writable(si)) {
                continue;
            }
            emit_static_root_desc_for_static_init(buf, si);
        }
        ncc_buffer_puts(buf,
            "        };\n"
            "        int __ncc_ct_writable_capture_rc = "
            "n00b_crt_capture_writable_static_roots(\n"
            "            __ncc_ct_writable_roots, ");
        ncc_buffer_printf(buf, "%dULL, ",
                          entry_context_writable_root_count(ctx));
        emit_c_string_literal(buf, ctx->captured_writable_image_path);
        ncc_buffer_puts(buf,
            ");\n"
            "        if (__ncc_ct_writable_capture_rc != 0) {\n"
            "            crc = __ncc_ct_writable_capture_rc;\n"
            "        }\n"
            "    }\n");
    }
}

static void
emit_static_init_prologue(ncc_buffer_t *buf,
                          const ncc_crt_entry_context_t *ctx)
{
    if (!entry_context_has_static_inits(ctx)) {
        return;
    }

    ncc_buffer_puts(buf,
        "    /* SEAM (WP-005 / D-024): prologue-evaluate bakeable static "
        "initializers before comptime_main. */\n");
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        ncc_buffer_printf(buf,
            "    if (crc == 0) {\n"
            "        crc = __ncc_static_init_prepare_%.*s();\n"
            "    }\n",
            (int)si->name.u8_bytes, si->name.data);
    }
}

static void
emit_static_init_value_capture(ncc_buffer_t *buf,
                               const ncc_crt_entry_context_t *ctx,
                               const char *phase,
                               const char *const *paths)
{
    if (!entry_context_has_static_inits(ctx) || !paths) {
        return;
    }

    ncc_buffer_printf(buf,
        "    /* SEAM (WP-005 / D-024): capture static-init %s values for "
        "driver comparison. */\n",
        phase ? phase : "unknown");
    for (int i = 0; i < ctx->meta->n_static_inits; i++) {
        const ncc_ct_static_init_t *si = &ctx->meta->static_inits[i];
        ncc_buffer_printf(buf,
            "    if (crc == 0) {\n"
            "        n00b_crt_static_root_desc_t "
            "__ncc_static_init_root_%.*s[] = {\n",
            (int)si->name.u8_bytes, si->name.data);
        emit_static_root_desc_for_static_init(buf, si);
        ncc_buffer_printf(buf,
            "        };\n"
            "        int __ncc_static_init_capture_%.*s = "
            "n00b_crt_capture_static_roots(\n"
            "            __ncc_static_init_root_%.*s, 1ULL, ",
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data);
        emit_c_string_literal(buf, paths[i]);
        ncc_buffer_printf(buf,
            ");\n"
            "        if (__ncc_static_init_capture_%.*s != 0) {\n"
            "            crc = __ncc_static_init_capture_%.*s;\n"
            "        }\n"
            "    }\n",
            (int)si->name.u8_bytes, si->name.data,
            (int)si->name.u8_bytes, si->name.data);
    }
}

static bool
emit_comptime_run_entry(ncc_buffer_t *buf, const ncc_crt_entry_context_t *ctx)
{
    if (entry_context_has_ro_roots(ctx) && !ctx->captured_image_path) {
        return false;
    }
    if (entry_context_has_writable_roots(ctx)
        && !ctx->captured_writable_image_path) {
        return false;
    }
    if (entry_context_has_static_inits(ctx)
        && (!ctx->static_init_snapshot_paths
            || !ctx->static_init_check_paths)) {
        return false;
    }

    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-crt-entry.c\"\n"
        "extern void n00b_crt_run_init_array(void);\n"
        "extern void n00b_init_core_simple(int argc, char **argv);\n"
        "extern void n00b_init_late(void);\n"
        "[[noreturn]] extern void n00b_exit(int rc);\n");
    if (entry_context_has_comptime_main(ctx)) {
        ncc_buffer_puts(buf,
            "extern int comptime_main(int argc, char **argv, char **envp);\n");
    }
    emit_static_init_helper_externs(buf, ctx);
    if (entry_context_has_roots(ctx)) {
        emit_static_root_desc_decl(buf);
        ncc_buffer_puts(buf,
            "extern int n00b_crt_capture_static_roots(\n"
            "    const n00b_crt_static_root_desc_t *roots,\n"
            "    unsigned long long root_count,\n"
            "    const char *path);\n");
        if (entry_context_has_writable_roots(ctx)) {
            ncc_buffer_puts(buf,
                "extern int n00b_crt_capture_writable_static_roots(\n"
                "    const n00b_crt_static_root_desc_t *roots,\n"
                "    unsigned long long root_count,\n"
                "    const char *path);\n");
        }
        emit_root_externs(buf, ctx);
    }
    ncc_buffer_puts(buf,
        "#if defined(__linux__)\n"
        "[[gnu::visibility(\"hidden\")]] void *__dso_handle = &__dso_handle;\n"
        "#endif\n"
        "\n"
        "/* PRE-RUNTIME CONTEXT: only raw C and declared runtime entry symbols\n"
        " * may be used before n00b_init_core_simple returns. No GC allocation,\n"
        " * n00b containers, conduits, or print APIs are available here. */\n"
        "[[noreturn]] void\n"
        "n00b_crt_main(int argc, char **argv, char **envp)\n"
        "{\n"
        "    /* D-047: preserve libc crt0 ordering for constructor tables. */\n"
        "    n00b_crt_run_init_array();\n"
        "    n00b_init_core_simple(argc, argv);\n"
        "    int crc = 0;\n");
    emit_static_init_prologue(buf, ctx);
    ncc_buffer_puts(buf,
        "    if (crc == 0) {\n"
        "        n00b_init_late();\n"
        "    }\n");
    emit_static_init_value_capture(buf, ctx, "prologue",
                                   ctx ? ctx->static_init_snapshot_paths
                                       : nullptr);
    if (entry_context_has_comptime_main(ctx)) {
        ncc_buffer_puts(buf,
            "    if (crc == 0) {\n"
            "        crc = comptime_main(argc, argv, envp);\n"
            "    }\n");
    }
    else {
        ncc_buffer_puts(buf,
            "    (void)envp;\n");
    }
    emit_static_init_value_capture(buf, ctx, "epilogue",
                                   ctx ? ctx->static_init_check_paths
                                       : nullptr);
    ncc_buffer_puts(buf,
        "    /* SEAM (WP-003): marshal/export comptime image capture runs\n"
        "     * here after comptime_main and before process exit. */\n");
    emit_capture_epilogue(buf, ctx);
    ncc_buffer_puts(buf,
        "    /* SEAM (WP-004 / D-010 / D-020): atomic failure cleanup and\n"
        "     * guard/degrade policy for comptime execution failures. */\n"
        "    n00b_exit(crc);\n"
        "}\n"
        "# line 1 \"ncc-generated-crt-entry-end.c\"\n");
    return true;
}

const char *
ncc_crt_emit_entry_ex(const ncc_opts_t *opts, ncc_crt_variant_t variant,
                      const ncc_crt_entry_context_t *ctx)
{
    if (!ncc_is_link_invocation(opts)) {
        return nullptr;
    }
    if (variant != NCC_CRT_VARIANT_DEGRADE
        && !entry_context_roots_supported(ctx)) {
        return nullptr;
    }

    ncc_free(last_entry_source);

    ncc_buffer_t *buf = ncc_buffer_empty();

    switch (variant) {
    case NCC_CRT_VARIANT_BASE:
        emit_base_entry(buf, variant, nullptr);
        break;
    case NCC_CRT_VARIANT_FINAL:
        emit_base_entry(buf, variant, ctx);
        break;
    case NCC_CRT_VARIANT_DEGRADE:
        emit_base_entry(buf, variant, ctx);
        break;
    case NCC_CRT_VARIANT_COMPTIME_RUN:
        if (!emit_comptime_run_entry(buf, ctx)) {
            ncc_buffer_free(buf);
            last_entry_source = nullptr;
            return nullptr;
        }
        break;
    default:
        ncc_buffer_free(buf);
        last_entry_source = nullptr;
        return nullptr;
    }

    last_entry_source = ncc_buffer_take(buf);

    return last_entry_source;
}

const char *
ncc_crt_emit_entry(const ncc_opts_t *opts, ncc_crt_variant_t variant)
{
    return ncc_crt_emit_entry_ex(opts, variant, nullptr);
}
