#include "xform/xform_static_object.h"

#include "lib/buffer.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

void
ncc_static_object_names_from_parts(ncc_static_object_names_t *out,
                                   const char *desc_name,
                                   const char *entry_name)
{
    snprintf(out->desc_name, sizeof(out->desc_name), "%s", desc_name);
    snprintf(out->entry_name, sizeof(out->entry_name), "%s", entry_name);

    uint64_t id = ncc_type_hash_u64(out->desc_name);
    snprintf(out->object_id, sizeof(out->object_id), "%" PRIu64 "ULL", id);
}

void
ncc_static_object_names_for_rstr(ncc_static_object_names_t *out, int uid)
{
    char desc_name[64];
    char entry_name[64];

    snprintf(desc_name, sizeof(desc_name), "_ncc_rsd_%d", uid);
    snprintf(entry_name, sizeof(entry_name), "_ncc_rse_%d", uid);
    ncc_static_object_names_from_parts(out, desc_name, entry_name);
}

void
ncc_static_object_names_for_array(ncc_static_object_names_t *out,
                                  const char *data_name)
{
    char desc_name[128];
    char entry_name[128];

    snprintf(desc_name, sizeof(desc_name), "%s_desc", data_name);
    snprintf(entry_name, sizeof(entry_name), "%s_entry", data_name);
    ncc_static_object_names_from_parts(out, desc_name, entry_name);
}

char *
ncc_static_object_typehash_expr(const char *type_name)
{
    uint64_t typehash = ncc_type_hash_u64(type_name);
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf, "%" PRIu64 "ULL", typehash);
    return ncc_buffer_take(buf);
}

const char *
ncc_static_object_entry_attr(ncc_xform_ctx_t *ctx)
{
    const char *attr = ncc_xform_get_data(ctx)->static_object_entry_attr;
    return attr ? attr : "";
}

void
ncc_static_object_slots_init(ncc_static_object_slots_t *out,
                             ncc_xform_ctx_t *ctx,
                             const ncc_static_object_names_t *names,
                             const char *typehash,
                             const char *flags,
                             const char *scan_kind,
                             const char *scan_cb,
                             const char *scan_user)
{
    *out = (ncc_static_object_slots_t){
        .typehash   = typehash,
        .desc_name  = names->desc_name,
        .entry_name = names->entry_name,
        .object_id  = names->object_id,
        .flags      = flags,
        .scan_kind  = scan_kind,
        .scan_cb    = scan_cb,
        .scan_user  = scan_user,
        .entry_attr = ncc_static_object_entry_attr(ctx),
    };
}

char *
ncc_static_object_expand_template(const char *template_kind,
                                  const char *tmpl,
                                  const char **args,
                                  int nargs)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    for (const char *p = tmpl; p && *p;) {
        if (*p != '$' || !isdigit((unsigned char)p[1])) {
            ncc_buffer_putc(buf, *p++);
            continue;
        }

        p++;
        int slot = 0;
        while (isdigit((unsigned char)*p)) {
            slot = slot * 10 + (*p - '0');
            p++;
        }

        if (slot < 0 || slot >= nargs) {
            fprintf(stderr,
                    "ncc: error: %s template references unavailable slot $%d\n",
                    template_kind ? template_kind : "static object",
                    slot);
            exit(1);
        }

        ncc_buffer_puts(buf, args[slot] ? args[slot] : "");
    }

    return ncc_buffer_take(buf);
}
