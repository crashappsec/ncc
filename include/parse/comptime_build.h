#pragma once

#include "internal/ncc_opts.h"
#include "parse/comptime_meta.h"
#include "util/platform.h"

#include <stdbool.h>

typedef struct {
    const char *const *user_inputs;
    int               n_user_inputs;
    const char       *output_file;
    const char *const *runtime_inputs;
    int               n_runtime_inputs;
    const char *const *ordered_link_args;
    int               n_ordered_link_args;
    const char *const *link_args;
    int               n_link_args;
    const char *const *comptime_argv;
    int               n_comptime_argv;
    const ncc_ct_aggregate_t *meta;
    const char       *captured_image_path;
    const char       *captured_writable_image_path;
} ncc_comptime_plan_t;

/**
 * Build a child-process argv vector for the comptime executable.
 *
 * @pre  program is non-null; args may be null only when n_args is 0.
 * @post returns a null-terminated ncc-owned argv vector whose first element is
 *       program followed by args in order.
 */
const char **ncc_collect_comptime_argv(const char *program,
                                       const char *const *args, int n_args);

/**
 * Run the WP-002 link-twice comptime build plan.
 *
 * @pre  opts and plan are non-null; host == target; plan->ordered_link_args
 *       replays the user's link operands in order; plan->runtime_inputs provide
 *       the accepted custom-entry runtime ABI.
 * @post returns 0 after successful comptime execution, optional captured-image
 *       object emission for read-only and writable roots, final link, and D-028
 *       section strip; returns non-zero and sets err_out when available if glue
 *       compilation, link #1, comptime execution, image object emission, link
 *       #2, or strip fails.
 */
int ncc_comptime_run_and_link(const ncc_opts_t *opts,
                              const ncc_comptime_plan_t *plan,
                              char **err_out);

/**
 * gcmap-prelink: read n00b_gcraw records from the link-input objects, generate
 * the typed GC type-map dictionary TU, compile it, and return its object path
 * (allocated in `tmp`, owned by the caller). *object_path_out is NULL (and the
 * call returns true) when no records were found. See gcmap_prelink.h.
 */
bool ncc_gcmap_prelink_build_object(const ncc_opts_t     *opts,
                                    const char *const    *objects,
                                    int                   n_objects,
                                    ncc_temp_workspace_t *tmp,
                                    char                **object_path_out,
                                    char                **err_out);

/**
 * gcmap standalone emit (--ncc-gcmap-emit=OUT): read n00b_gcraw from the given
 * objects/archives, generate+compile the typed dictionary TU, and write the
 * object directly to `out_path`. When no records are found, writes a valid
 * object defining EMPTY dictionary symbols (count==0), so the build always has a
 * file to link. For build systems where ncc does not drive the final link.
 * Returns false (sets *err_out) on error.
 */
bool ncc_gcmap_emit_to_path(const ncc_opts_t  *opts,
                            const char *const *objects,
                            int                n_objects,
                            const char        *out_path,
                            char             **err_out);

/**
 * Link a runtime-degraded comptime program without executing comptime at build.
 *
 * @pre  opts and plan are non-null; plan->meta has a comptime_main record, one
 *       or more degrade_ok static-initializer records, or both. The caller
 *       selected this route via --ncc-no-comptime, target-mismatch optional
 *       comptime_main policy, or cross-target static-init degrade policy.
 * @post returns 0 after generating the runtime-degrade entry, optional
 *       static-init degrade table object, final link, and D-028 section strip;
 *       returns non-zero and sets err_out when available if glue compilation,
 *       degrade-object compilation, link, or strip fails.
 */
int ncc_comptime_degrade_and_link(const ncc_opts_t *opts,
                                  const ncc_comptime_plan_t *plan,
                                  char **err_out);
