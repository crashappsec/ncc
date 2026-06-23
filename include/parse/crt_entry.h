#pragma once

#include "internal/ncc_opts.h"
#include "parse/comptime_meta.h"

#include <stdbool.h>

typedef enum {
    NCC_CRT_VARIANT_BASE,
    NCC_CRT_VARIANT_COMPTIME_RUN,
    NCC_CRT_VARIANT_FINAL,
    NCC_CRT_VARIANT_DEGRADE,
} ncc_crt_variant_t;

typedef struct {
    const ncc_ct_aggregate_t *meta;
    const char               *captured_image_path;
    const char               *captured_writable_image_path;
    const char *const        *static_init_snapshot_paths;
    const char *const        *static_init_check_paths;
} ncc_crt_entry_context_t;

/**
 * True when this ncc invocation links a program rather than stopping after
 * preprocessing or compilation.
 *
 * @pre  opts may be null.
 * @post returns false for no-input, -c, -E, -S, -fsyntax-only, and
 *       dependency-only modes; true for ordinary link mode.
 */
bool ncc_is_link_invocation(const ncc_opts_t *opts);

/**
 * Emit the generated libc-less C entry source for `n00b_crt_main`.
 *
 * @pre  opts describes a link invocation.
 * @post returns an ncc-owned buffer valid until the next ncc_crt_emit_entry call.
 *       BASE and FINAL emit the standard user-main entry; COMPTIME_RUN emits the
 *       link-time comptime driver entry; DEGRADE emits a runtime fallback entry
 *       that calls comptime_main before main without synthetic comptime storage.
 */
const char *ncc_crt_emit_entry(const ncc_opts_t *opts,
                               ncc_crt_variant_t variant);

/**
 * Emit a generated CRT entry with optional WP-003 comptime image context.
 *
 * @pre  opts describes a link invocation; ctx may be null for context-free
 *       BASE/FINAL/COMPTIME_RUN emission.
 * @post FINAL emits the relocation prologue and, when ctx->meta has comptime
 *       roots, assigns relocated read-only and writable roots back to external
 *       pointer variables. COMPTIME_RUN emits the capture epilogue when ctx
 *       supplies roots and the matching capture paths; when static-init
 *       snapshot/check paths are present, it also captures each static-init root
 *       before and after comptime_main for D-024 value comparison by the
 *       driver. DEGRADE ignores comptime image context and reuses the user
 *       object's comptime globals directly.
 */
const char *ncc_crt_emit_entry_ex(const ncc_opts_t *opts,
                                  ncc_crt_variant_t variant,
                                  const ncc_crt_entry_context_t *ctx);
