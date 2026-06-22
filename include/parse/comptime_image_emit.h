#pragma once

#include "internal/ncc_opts.h"

#include <stdbool.h>

#define NCC_CT_IMAGE_SECTION_ELF ".n00b.ctimage"
#define NCC_CT_IMAGE_SECTION_MACHO_SEG "__DATA"
#define NCC_CT_IMAGE_SECTION_MACHO_SECT "__n00b_ctimg"
#define NCC_CT_IMAGE_SECTION_MACHO "__DATA,__n00b_ctimg"
#define NCC_CT_IMAGE_SECTION_PE ".n00bcti"

#define NCC_CT_WRITABLE_IMAGE_SECTION_ELF ".n00b.ctwimg"
#define NCC_CT_WRITABLE_IMAGE_SECTION_MACHO_SEG "__DATA"
#define NCC_CT_WRITABLE_IMAGE_SECTION_MACHO_SECT "__n00b_ctwimg"
#define NCC_CT_WRITABLE_IMAGE_SECTION_MACHO "__DATA,__n00b_ctwimg"
#define NCC_CT_WRITABLE_IMAGE_SECTION_PE ".n00bctw"

/**
 * Wrap captured comptime image bytes in a linkable object file.
 *
 * @pre  opts, opts->compiler, image_bytes_path, and out_obj_path are non-null.
 *       image_bytes_path names an existing non-empty image byte file.
 * @post returns true after compiling a generated C blob object whose platform
 *       image section contains exactly the input bytes; returns false and sets
 *       err_out when available on read, write, or compiler failure.
 */
bool ncc_emit_image_object(const ncc_opts_t *opts, const char *image_bytes_path,
                           const char *out_obj_path, char **err_out);

/**
 * Wrap captured writable comptime image bytes in a linkable object file.
 *
 * @pre  opts, opts->compiler, image_bytes_path, and out_obj_path are non-null.
 *       image_bytes_path names an existing non-empty writable image byte file.
 * @post returns true after compiling a generated C blob object whose platform
 *       writable image section contains exactly the input bytes; returns false
 *       and sets err_out when available on read, write, or compiler failure.
 */
bool ncc_emit_writable_image_object(const ncc_opts_t *opts,
                                    const char *image_bytes_path,
                                    const char *out_obj_path,
                                    char **err_out);
