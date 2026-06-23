#pragma once

#include "internal/ncc_opts.h"
#include "lib/string.h"
#include "ncc.h"

#include <stdbool.h>
#include <stdint.h>

#define NCC_CT_FORMAT_VERSION 4
#define NCC_CT_MAGIC "N0CT"

#define NCC_CT_SECTION_ELF ".n00b.comptime"
#define NCC_CT_SECTION_MACHO_SEG "__DATA"
#define NCC_CT_SECTION_MACHO_SECT "__n00b_ct"
#define NCC_CT_SECTION_MACHO "__DATA,__n00b_ct"
#define NCC_CT_SECTION_PE ".n00bct"

typedef enum {
    NCC_CT_REC_END           = 0,
    NCC_CT_REC_COMPTIME_MAIN = 1,
    NCC_CT_REC_VAR           = 2,
    NCC_CT_REC_STATIC_INIT   = 3,
} ncc_ct_rec_kind_t;

typedef struct {
    uint8_t argc;
    bool    has_argv;
    bool    has_envp;
} ncc_ct_sig_t;

#define NCC_CT_MAIN_FLAG_OPTIONAL UINT8_C(0x01)

typedef struct {
    ncc_string_t name;
    uint64_t     typehash;
    uint8_t      linkage; // 0 = internal, 1 = external
    uint8_t      flags;
} ncc_ct_var_t;

#define NCC_CT_VAR_FLAG_POINTER_ROOT UINT8_C(0x01)

#define NCC_CT_STATIC_INIT_CONST_RO UINT8_C(0x00)  // const -> RELRO image
#define NCC_CT_STATIC_INIT_WRITABLE UINT8_C(0x01)  // non-const -> writable image
#define NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT UINT8_C(0x01)

typedef struct {
    ncc_string_t name;
    uint64_t     typehash;
    uint8_t      kind;
    uint8_t      flags;
    // 1 when a later cross-target build may lower this initializer to runtime
    // init instead of baking a host-shaped image.
    uint8_t      degrade_ok;
} ncc_ct_static_init_t;

typedef struct {
    bool                  has_comptime_main;
    ncc_ct_sig_t          main_sig;
    uint8_t               main_flags;
    ncc_ct_var_t         *vars;
    int                   n_vars;
    int                   vars_cap;
    ncc_ct_static_init_t *static_inits;
    int                   n_static_inits;
    int                   static_inits_cap;
    int                   n_objects_scanned;
} ncc_ct_aggregate_t;

typedef struct {
    ncc_ct_rec_kind_t      kind;
    ncc_ct_sig_t           sig;
    uint8_t                main_flags;
    ncc_ct_var_t           var;
    ncc_ct_static_init_t   static_init;
} ncc_ct_rec_t;

typedef struct {
    ncc_ct_rec_t *records;
    int           n_records;
    int           records_cap;
    int           n_objects_scanned;
} ncc_ct_rec_list_t;

/**
 * Build generated C declaring this TU's D-028/D-007 comptime metadata section.
 *
 * Metadata format v4 extends COMPTIME_MAIN with a flags byte and static-init
 * records with a root-flags byte. The defined main flag is
 * NCC_CT_MAIN_FLAG_OPTIONAL, persisted from valid [[n00b::optional]] use so
 * object/archive links retain the source-level skip policy. The defined
 * static-init flag is NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT. Static inits
 * without that flag are value roots captured through a managed copy and applied
 * back into the static object by the generated CRT entry.
 *
 * @pre  vars is null when n_vars is 0; every var name has stable storage for
 *       the duration of the call; main_flags contains only
 *       NCC_CT_MAIN_FLAG_* bits.
 * @post returns null when sig is null and n_vars is 0; otherwise returns an
 *       ncc-owned source fragment containing a retained section byte array.
 */
const char *ncc_ct_emit_section_decl(const ncc_opts_t *opts,
                                     const ncc_ct_sig_t *sig,
                                     uint8_t main_flags,
                                     const ncc_ct_var_t *vars, int n_vars);

/**
 * Build generated C declaring this TU's D-028 metadata section, including
 * WP-005 static-init records.
 *
 * @pre  static_inits is null when n_static_inits is 0; every name has stable
 *       storage for the duration of the call.
 * @post returns null when no records are requested; otherwise returns an
 *       ncc-owned source fragment containing a retained section byte array.
 */
const char *ncc_ct_emit_section_decl_ex(
    const ncc_opts_t *opts, const ncc_ct_sig_t *sig, uint8_t main_flags,
    const ncc_ct_var_t *vars, int n_vars,
    const ncc_ct_static_init_t *static_inits, int n_static_inits);

/**
 * Release records and any owned record payloads.
 *
 * @pre  list may be null or zero-initialized.
 * @post list is reset to all-zero state.
 */
void ncc_ct_rec_list_free(ncc_ct_rec_list_t *list);

/**
 * Release aggregate-owned variable records.
 *
 * @pre  agg may be null or zero-initialized.
 * @post agg is reset to all-zero state.
 */
void ncc_ct_aggregate_free(ncc_ct_aggregate_t *agg);

/**
 * Read and parse an object's D-028 comptime metadata section, if present.
 * Archives whose raw bytes contain the D-028 magic are expanded and each
 * member is scanned as an object; archives without the magic are a successful
 * no-op because no member can contain a valid D-028 section.
 *
 * @pre  obj_path names an object/archive readable by llvm-objcopy/objcopy
 *       and, for archives, llvm-ar/ar; out is non-null and initialized by
 *       the caller.
 * @post records from a present metadata section are appended to out;
 *       objects without the section append no records; out->n_objects_scanned
 *       is incremented on successful object inspection. On failure err_out
 *       receives a caller-owned diagnostic when non-null.
 */
bool ncc_ct_read_object(const ncc_opts_t *opts, const char *obj_path,
                        ncc_ct_rec_list_t *out, char **err_out);

/**
 * Aggregate per-object comptime records into link-level metadata.
 *
 * @pre  recs and out are non-null; out is zero-initialized.
 * @post out receives the unique comptime_main signature and flags, all unique
 *       comptime variables, and the scanned-object count. Conflicting main
 *       signatures/flags or variable records fail with err_out set when
 *       non-null.
 */
bool ncc_ct_aggregate(const ncc_ct_rec_list_t *recs,
                      ncc_ct_aggregate_t *out, char **err_out);

/**
 * Remove the D-028 comptime metadata section from an object or binary in place.
 *
 * @pre  binary_path names a writable object/binary path readable by
 *       llvm-objcopy/objcopy.
 * @post any known local comptime metadata section name has been removed when
 *       present; absence is a successful no-op. On failure err_out receives a
 *       caller-owned diagnostic when non-null.
 */
bool ncc_ct_strip_section(const ncc_opts_t *opts, const char *binary_path,
                          char **err_out);
