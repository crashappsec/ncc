#pragma once

#include "xform/transform.h"

typedef struct {
    char desc_name[128];
    char entry_name[128];
    char object_id[32];
} ncc_static_object_names_t;

typedef struct {
    const char *typehash;
    const char *desc_name;
    const char *entry_name;
    const char *object_id;
    const char *flags;
    const char *scan_kind;
    const char *scan_cb;
    const char *scan_user;
    const char *entry_attr;
} ncc_static_object_slots_t;

void ncc_static_object_names_from_parts(ncc_static_object_names_t *out,
                                        const char *desc_name,
                                        const char *entry_name);
void ncc_static_object_names_for_rstr(ncc_static_object_names_t *out, int uid);
void ncc_static_object_names_for_array(ncc_static_object_names_t *out,
                                       const char *data_name);

char *ncc_static_object_typehash_expr(const char *type_name);
const char *ncc_static_object_entry_attr(ncc_xform_ctx_t *ctx);
void ncc_static_object_slots_init(ncc_static_object_slots_t *out,
                                  ncc_xform_ctx_t *ctx,
                                  const ncc_static_object_names_t *names,
                                  const char *typehash,
                                  const char *flags,
                                  const char *scan_kind,
                                  const char *scan_cb,
                                  const char *scan_user);
char *ncc_static_object_expand_template(const char *template_kind,
                                        const char *tmpl,
                                        const char **args,
                                        int nargs);
