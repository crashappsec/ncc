#include "xform/xform_static_object.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/type_normalize.h"
#include "util/platform.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *
copy_cstr(const char *s)
{
    if (!s) {
        s = "";
    }
    size_t len = strlen(s);
    char  *out = ncc_alloc_size(1, len + 1);
    memcpy(out, s, len + 1);
    return out;
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

static char *
read_text_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len < 0) {
        fclose(f);
        return nullptr;
    }

    char  *buf   = ncc_alloc_size(1, (size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread]   = '\0';
    fclose(f);
    return buf;
}

static char *
normalize_path_text(const char *path)
{
    char *out = copy_cstr(path ? path : "");
    for (char *p = out; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return out;
}

static char *
path_basename_copy(const char *path)
{
    if (!path || !*path) {
        return copy_cstr("unknown");
    }

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }

    return copy_cstr(*base ? base : "unknown");
}

static char *
derive_namespace_from_dir(const char *dir)
{
    char *base = path_basename_copy(dir);
    if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        ncc_free(base);
        return copy_cstr("ncc.default");
    }

    for (char *p = base; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-'
              || *p == '.')) {
            *p = '-';
        }
    }

    return base;
}

static char *
trim_in_place(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    return s;
}

static bool
toml_split_assignment(char *line, char **key_out, char **value_out)
{
    char *comment = strchr(line, '#');
    if (comment) {
        *comment = '\0';
    }

    char *eq = strchr(line, '=');
    if (!eq) {
        return false;
    }

    *eq = '\0';
    *key_out   = trim_in_place(line);
    *value_out = trim_in_place(eq + 1);
    return **key_out != '\0';
}

static char *
toml_unquote_cstr(const char *value)
{
    if (!value) {
        return nullptr;
    }

    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    if (*value != '"') {
        return nullptr;
    }
    value++;

    ncc_buffer_t *buf = ncc_buffer_empty();
    for (const char *p = value; *p; p++) {
        if (*p == '"') {
            return ncc_buffer_take(buf);
        }
        if (*p == '\\') {
            p++;
            if (!*p) {
                break;
            }
            switch (*p) {
            case 'n':
                ncc_buffer_putc(buf, '\n');
                break;
            case 'r':
                ncc_buffer_putc(buf, '\r');
                break;
            case 't':
                ncc_buffer_putc(buf, '\t');
                break;
            case '"':
            case '\\':
                ncc_buffer_putc(buf, *p);
                break;
            default:
                ncc_buffer_putc(buf, *p);
                break;
            }
            continue;
        }
        ncc_buffer_putc(buf, *p);
    }

    ncc_buffer_free(buf);
    return nullptr;
}

typedef struct {
    char *path;
    char *namespace_id;
} namespace_exception_t;

typedef struct {
    char                  *default_namespace;
    namespace_exception_t *exceptions;
    size_t                 len;
    size_t                 cap;
} namespace_config_t;

static void
namespace_config_add_exception(namespace_config_t *config, char *path,
                               char *namespace_id)
{
    if (!path || !namespace_id) {
        ncc_free(path);
        ncc_free(namespace_id);
        return;
    }

    if (config->len == config->cap) {
        size_t new_cap = config->cap ? config->cap * 2 : 4;
        config->exceptions = ncc_realloc(
            config->exceptions, new_cap * sizeof(namespace_exception_t));
        config->cap = new_cap;
    }

    config->exceptions[config->len++] = (namespace_exception_t){
        .path         = path,
        .namespace_id = namespace_id,
    };
}

static void
namespace_config_free(namespace_config_t *config)
{
    ncc_free(config->default_namespace);
    for (size_t i = 0; i < config->len; i++) {
        ncc_free(config->exceptions[i].path);
        ncc_free(config->exceptions[i].namespace_id);
    }
    ncc_free(config->exceptions);
}

static namespace_config_t
parse_namespace_config(const char *path)
{
    namespace_config_t config = {0};
    char              *text   = read_text_file(path);
    if (!text) {
        return config;
    }

    bool  in_exception = false;
    char *cur_path     = nullptr;
    char *cur_ns       = nullptr;

    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }

        char *trimmed = trim_in_place(line);
        if (!*trimmed || *trimmed == '#') {
            line = next;
            continue;
        }

        if (strcmp(trimmed, "[[exceptions]]") == 0) {
            namespace_config_add_exception(&config, cur_path, cur_ns);
            cur_path     = nullptr;
            cur_ns       = nullptr;
            in_exception = true;
            line         = next;
            continue;
        }

        char *key   = nullptr;
        char *value = nullptr;
        if (!toml_split_assignment(trimmed, &key, &value)) {
            line = next;
            continue;
        }

        char *decoded = toml_unquote_cstr(value);
        if (!decoded) {
            line = next;
            continue;
        }

        if (in_exception) {
            if (strcmp(key, "path") == 0) {
                ncc_free(cur_path);
                cur_path = normalize_path_text(decoded);
                ncc_free(decoded);
            }
            else if (strcmp(key, "namespace") == 0) {
                ncc_free(cur_ns);
                cur_ns = decoded;
            }
            else {
                ncc_free(decoded);
            }
        }
        else if (strcmp(key, "namespace") == 0
                 || strcmp(key, "default") == 0) {
            ncc_free(config.default_namespace);
            config.default_namespace = decoded;
        }
        else {
            ncc_free(decoded);
        }

        line = next;
    }

    namespace_config_add_exception(&config, cur_path, cur_ns);
    ncc_free(text);
    return config;
}

static bool
source_matches_exception(const char *source, const char *prefix)
{
    if (!source || !prefix || !*prefix) {
        return false;
    }

    size_t len = strlen(prefix);
    if (strcmp(source, prefix) == 0) {
        return true;
    }

    if (prefix[len - 1] == '/') {
        return strncmp(source, prefix, len) == 0;
    }

    return strncmp(source, prefix, len) == 0 && source[len] == '/';
}

static const char *
namespace_for_source(namespace_config_t *config, const char *source_rel)
{
    const char *best     = config->default_namespace;
    size_t      best_len = 0;

    for (size_t i = 0; i < config->len; i++) {
        namespace_exception_t *ex = &config->exceptions[i];
        if (!source_matches_exception(source_rel, ex->path)) {
            continue;
        }

        size_t len = strlen(ex->path);
        if (len >= best_len) {
            best     = ex->namespace_id;
            best_len = len;
        }
    }

    return best;
}

static char *
relative_source_id(const char *source_path, const char *root_path)
{
    char *src  = normalize_path_text(source_path);
    char *root = normalize_path_text(root_path);

    size_t root_len = strlen(root);
    const char *rel = src;
    if (root_len > 0 && strncmp(src, root, root_len) == 0
        && (src[root_len] == '\0' || src[root_len] == '/')) {
        rel = src + root_len;
        if (*rel == '/') {
            rel++;
        }
    }

    char *out = copy_cstr(*rel ? rel : src);
    ncc_free(src);
    ncc_free(root);
    return out;
}

static char *
find_namespace_file(const char *source_path, char **root_out)
{
    *root_out = nullptr;

    char *dir = ncc_platform_dirname(source_path);

    while (dir && *dir) {
        char *candidate = ncc_platform_join_path(dir, ".namespace.toml");
        if (candidate && file_exists(candidate)) {
            *root_out = copy_cstr(dir);
            ncc_free(dir);
            return candidate;
        }
        ncc_free(candidate);

        char *parent = ncc_platform_dirname(dir);
        bool  same   = !parent || strcmp(parent, dir) == 0;
        if (same) {
            ncc_free(parent);
            break;
        }
        ncc_free(dir);
        dir = parent;
    }

    ncc_free(dir);
    return nullptr;
}

static char *
source_path_for_site(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site)
{
    const char *site_file = ncc_xform_first_leaf_file(site);
    if (!site_file || !*site_file) {
        site_file = ncc_xform_get_data(ctx)->input_file;
    }
    if (!site_file || !*site_file) {
        return copy_cstr("unknown.c");
    }

    char *real = ncc_platform_realpath(site_file);
    return real ? real : copy_cstr(site_file);
}

static char *
write_default_namespace_file(const char *source_path, char **root_out)
{
    char *dir       = ncc_platform_dirname(source_path);
    char *ns        = derive_namespace_from_dir(dir);
    ncc_buffer_t *buf = ncc_buffer_empty();

    ncc_buffer_printf(buf,
                      "# Generated by ncc when stable static identities were "
                      "first required.\n"
                      "namespace = \"%s\"\n",
                      ns);
    char *contents = ncc_buffer_take(buf);
    char *path     = ncc_platform_join_path(dir, ".namespace.toml");
    char *err      = nullptr;

    if (!ncc_platform_write_file(path, contents, strlen(contents), &err)) {
        fprintf(stderr,
                "ncc: error: failed to write generated namespace metadata "
                "'%s': %s\n",
                path, err ? err : "unknown error");
        ncc_free(err);
        exit(1);
    }

    *root_out = copy_cstr(dir);
    ncc_free(contents);
    ncc_free(ns);
    ncc_free(dir);
    return path;
}

static void
identity_context(ncc_xform_ctx_t *ctx, ncc_parse_tree_t *site,
                 char **namespace_out, char **source_id_out)
{
    char *source_path = source_path_for_site(ctx, site);
    char *root        = nullptr;
    char *meta_path   = find_namespace_file(source_path, &root);

    if (!meta_path
        && ncc_xform_get_data(ctx)->static_identity_generate_namespace) {
        meta_path = write_default_namespace_file(source_path, &root);
    }

    char *source_id = nullptr;
    char *namespace_id = nullptr;
    if (meta_path) {
        namespace_config_t config = parse_namespace_config(meta_path);
        source_id = relative_source_id(source_path, root);
        const char *selected = namespace_for_source(&config, source_id);
        namespace_id = selected ? copy_cstr(selected)
                                : derive_namespace_from_dir(root);
        namespace_config_free(&config);
    }
    else {
        char *source_dir = ncc_platform_dirname(source_path);
        source_id        = path_basename_copy(source_path);
        namespace_id     = copy_cstr("ncc.default");
        ncc_free(source_dir);
    }

    *namespace_out = namespace_id;
    *source_id_out = source_id;

    ncc_free(meta_path);
    ncc_free(root);
    ncc_free(source_path);
}

static char *
c_string_literal(const char *s)
{
    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_putc(buf, '"');

    for (const unsigned char *p = (const unsigned char *)(s ? s : "");
         *p; p++) {
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
            if (*p >= 0x20 && *p < 0x7f) {
                ncc_buffer_putc(buf, (char)*p);
            }
            else {
                ncc_buffer_printf(buf, "\\%03o", (unsigned)*p);
            }
            break;
        }
    }

    ncc_buffer_putc(buf, '"');
    return ncc_buffer_take(buf);
}

static uint64_t
site_semantic_hash(ncc_parse_tree_t *site)
{
    ncc_string_t text = ncc_xform_node_to_text(site);
    uint64_t     hash = ncc_type_hash_u64(text.data ? text.data : "");
    ncc_free(text.data);
    return hash;
}

char *
ncc_static_object_identity_namespace(ncc_xform_ctx_t *ctx,
                                     ncc_parse_tree_t *site)
{
    char *namespace_id = nullptr;
    char *source_id    = nullptr;
    identity_context(ctx, site, &namespace_id, &source_id);
    ncc_free(source_id);
    return namespace_id;
}

char *
ncc_static_object_identity_key(ncc_xform_ctx_t *ctx,
                               const char *identity_kind_key,
                               ncc_parse_tree_t *site,
                               const char *typehash,
                               const char *identity_len)
{
    char    *namespace_id = nullptr;
    char    *source_id    = nullptr;
    uint32_t line         = 0;
    uint32_t col          = 0;

    identity_context(ctx, site, &namespace_id, &source_id);
    ncc_xform_first_leaf_pos(site, &line, &col);
    uint64_t semantic_hash = site_semantic_hash(site);

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf, "%s:%s:%u:%u:t=%s:len=%s:h=%" PRIu64,
                      identity_kind_key ? identity_kind_key : "ncc-static",
                      source_id ? source_id : "unknown.c",
                      line, col,
                      typehash ? typehash : "0",
                      identity_len ? identity_len : "0",
                      semantic_hash);
    char *key = ncc_buffer_take(buf);

    ncc_free(namespace_id);
    ncc_free(source_id);
    return key;
}

void
ncc_static_object_names_from_parts(ncc_static_object_names_t *out,
                                   const char *desc_name,
                                   const char *entry_name)
{
    snprintf(out->desc_name, sizeof(out->desc_name), "%s", desc_name);
    snprintf(out->entry_name, sizeof(out->entry_name), "%s", entry_name);
    snprintf(out->identity_name, sizeof(out->identity_name), "%s_id",
             desc_name);

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

// Like ncc_static_object_typehash_expr, but hashes the POINTER form of the
// type. The runtime type registry and heap objects' dynamic metadata records
// key types by typehash(T*) (the reference form), so a static object's
// descriptor must carry typehash(T*) too — otherwise n00b_type_info_for()
// can't resolve a static object to the same type_info as its heap twin and
// the formatter falls through to the generic pointer route. Callers whose
// configured type name is spelled as a value (e.g. "n00b_string_t") need this;
// names already ending in '*' are hashed as-is. (xform_static_image.c emits the
// pointer form directly via "%s*"; this is the shared, '*'-idempotent helper.)
char *
ncc_static_object_ptr_typehash_expr(const char *type_name)
{
    if (!type_name) {
        type_name = "";
    }

    size_t len = strlen(type_name);
    if (len > 0 && type_name[len - 1] == '*') {
        return ncc_static_object_typehash_expr(type_name);
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_printf(buf, "%s*", type_name);
    char *ptr_type = ncc_buffer_take(buf);
    char *expr     = ncc_static_object_typehash_expr(ptr_type);
    ncc_free(ptr_type);
    return expr;
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
                             const char *scan_user,
                             const char *identity_kind_expr,
                             const char *identity_kind_key,
                             ncc_parse_tree_t *identity_site,
                             const char *identity_len)
{
    char *namespace_id = ncc_static_object_identity_namespace(ctx,
                                                             identity_site);
    char *object_key = ncc_static_object_identity_key(ctx, identity_kind_key,
                                                      identity_site,
                                                      typehash,
                                                      identity_len);
    char *namespace_lit = c_string_literal(namespace_id);
    char *object_key_lit = c_string_literal(object_key);

    ncc_buffer_t *decl = ncc_buffer_empty();
    ncc_buffer_printf(decl,
                      "static const n00b_static_identity_t %s={"
                      ".version=1u,"
                      ".kind=%s,.namespace_id=%s,.object_key=%s};",
                      names->identity_name,
                      identity_kind_expr ? identity_kind_expr
                                         : "N00B_STATIC_IDENTITY_NONE",
                      namespace_lit, object_key_lit);
    ncc_buffer_t *expr = ncc_buffer_empty();
    ncc_buffer_printf(expr, "&%s", names->identity_name);

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
        .identity_decl = ncc_buffer_take(decl),
        .identity_expr = ncc_buffer_take(expr),
    };

    ncc_free(namespace_id);
    ncc_free(object_key);
    ncc_free(namespace_lit);
    ncc_free(object_key_lit);
}

void
ncc_static_object_slots_cleanup(ncc_static_object_slots_t *slots)
{
    if (!slots) {
        return;
    }
    ncc_free(slots->identity_decl);
    ncc_free(slots->identity_expr);
    slots->identity_decl = nullptr;
    slots->identity_expr = nullptr;
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
