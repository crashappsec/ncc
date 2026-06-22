#pragma once

#include "internal/ncc_opts.h"
#include "parse/comptime_meta.h"
#include "util/platform.h"

#include <stdbool.h>

/**
 * Return the build host triple used by the comptime cross-target guard.
 *
 * @pre  called on a supported ncc host.
 * @post returns a static string in "<arch>-unknown-<os>" form where arch/os are
 *       normalized for guard comparison.
 */
const char *ncc_host_triple(void);

/**
 * Return whether clang passthrough target arguments name the build host.
 *
 * Recognized target spellings are `-target T`, `--target T`,
 * `-target=T`, `--target=T`, Darwin-style `-arch ARCH`, and standalone
 * explicit triples that start with a known arch and contain a known OS.
 * Vendor/ABI components are ignored; absence of target arguments means host.
 *
 * @pre  @p opts may be null.
 * @post returns true iff no non-host target is named, or the named target's
 *       normalized OS+arch equals the build host.
 */
bool ncc_target_is_host(const ncc_opts_t *opts);

/**
 * Enforce D-008 before a comptime build-time run is attempted.
 *
 * @pre  @p opts and @p agg describe the link being routed.
 * @post returns false and sets @p err_out when a comptime_main is present,
 *       the target is not the host, the main record is not optional, and the
 *       user did not request --ncc-no-comptime. Otherwise returns true.
 */
bool ncc_comptime_guard_check(const ncc_opts_t *opts,
                              const ncc_ct_aggregate_t *agg,
                              char **err_out);

typedef struct {
    bool target_is_host;
    bool comptime_main_degrade;
    bool static_init_degrade;
} ncc_comptime_degrade_route_t;

/**
 * Classify whether comptime work should run on the host or degrade to runtime.
 *
 * Static initializers degrade when --ncc-no-comptime is set or the link target
 * is not the build host. Optional comptime_main degrades for --ncc-no-comptime
 * and for target mismatch.
 *
 * @pre  @p opts and @p agg may be null.
 * @post returns a pure routing decision without validating whether static
 *       initializers are eligible for runtime lowering.
 */
ncc_comptime_degrade_route_t
ncc_comptime_degrade_route(const ncc_opts_t *opts,
                           const ncc_ct_aggregate_t *agg);

/**
 * Validate that all static initializers in an aggregate may degrade to runtime.
 *
 * @pre  @p agg may be null; @p err_out may be null.
 * @post returns false and sets @p err_out when a static initializer lacks a
 *       runtime-lowering path.
 */
bool ncc_static_init_degrade_allowed(const ncc_ct_aggregate_t *agg,
                                     char **err_out);

typedef enum {
    NCC_CT_RUN_OK,
    NCC_CT_RUN_EXIT,
    NCC_CT_RUN_SIGNAL,
    NCC_CT_RUN_EXCEPTION,
    NCC_CT_RUN_LAUNCH,
} ncc_ct_run_status_t;

/**
 * Convert a child-process result into a comptime-run status category.
 *
 * @pre  @p proc is a completed result from ncc_process_run, or null for a
 *       launch failure.
 * @post returns NCC_CT_RUN_OK only for normal exit status 0.
 */
ncc_ct_run_status_t ncc_comptime_run_status(const ncc_process_result_t *proc);

/**
 * Format a D-020 category-distinct comptime run failure diagnostic.
 *
 * @pre  @p st is not NCC_CT_RUN_OK.
 * @post returns a caller-owned message released with ncc_free().
 */
char *ncc_comptime_run_failure_message(ncc_ct_run_status_t st, int code);

/**
 * Atomically publish a successfully linked comptime output.
 *
 * @pre  @p temp_path names the completed temporary output beside
 *       @p output_file; @p output_file is the requested final path.
 * @post on success, @p temp_path has atomically replaced @p output_file. On
 *       failure, any pre-existing @p output_file is untouched.
 */
bool ncc_comptime_emit_output_atomic(const char *output_file,
                                     const char *temp_path,
                                     char **err_out);
