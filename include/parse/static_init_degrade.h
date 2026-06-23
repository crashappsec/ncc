#pragma once

#include "internal/ncc_opts.h"
#include "parse/comptime_meta.h"

#include <stdbool.h>

#define NCC_STATIC_INIT_DEGRADE_SECTION_ELF "n00b_sinit"
#define NCC_STATIC_INIT_DEGRADE_SECTION_MACHO "__DATA,n00b_sinit"
#define NCC_STATIC_INIT_DEGRADE_SECTION_MACHO_SECT "n00b_sinit"
#define NCC_STATIC_INIT_DEGRADE_SECTION_PE ".n00bsi$m"

/**
 * Emit a target-shaped object containing runtime-degrade static-init entries.
 *
 * @pre  opts, opts->compiler, meta, and out_obj_path are non-null. Every
 *       static initializer in meta is marked degrade_ok and has a C-identifier
 *       name matching its transformed helper symbol.
 * @post returns true after compiling an object whose platform section contains
 *       one callback per static initializer. Each callback calls the matching
 *       __ncc_static_init_prepare_<name>() helper in the user object and
 *       propagates its integer status. Returns false and sets err_out when
 *       metadata is invalid or compilation fails.
 */
bool ncc_emit_static_init_degrade_object(const ncc_opts_t *opts,
                                         const ncc_ct_aggregate_t *meta,
                                         const char *out_obj_path,
                                         char **err_out);
