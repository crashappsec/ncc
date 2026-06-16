#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && defined(_MSC_VER)
static char *
ncc_strtok_r(char *str, const char *delim, char **saveptr)
{
    return strtok_s(str, delim, saveptr);
}
#else
static char *
ncc_strtok_r(char *str, const char *delim, char **saveptr)
{
    return strtok_r(str, delim, saveptr);
}
#endif

typedef struct {
    char    *key_expr;
    char    *value_expr;
    uint64_t hash_lo;
    uint64_t hash_hi;
} dict_pair_t;

typedef struct {
    char  *type;
    char  *type_hash;
    char  *prefix;
    char  *entry_attr;
    char  *container_kind;
    char  *container_target;
    char  *element_type;
    char  *element_type_hash;
    char  *data_type_hash;
    char  *identity_namespace;
    char  *identity_object_key;
    char  *identity_payload_key;
    int    readonly;
    size_t len;
    size_t cap;
    char  *element_scan_kind;
    char  *element_scan_cb;
    char  *element_scan_user;
    char  *element_shape_decl;
    char  *payload_hex;
    size_t payload_len;
    char **cinit_items;
    size_t cinit_len;
    size_t cinit_cap;
    // Dict-specific (mirror of n00b helper minimal slice).
    char        *key_type;
    char        *value_type;
    char        *key_scan_kind;
    char        *value_scan_kind;
    char        *key_scan_cb;
    char        *value_scan_cb;
    char        *key_scan_user;
    char        *value_scan_user;
    char        *key_shape_decl;
    char        *value_shape_decl;
    int          skip_obj_hash;
    dict_pair_t *dict_pairs;
    size_t       dict_pair_count;
    size_t       dict_pair_cap;
    // WP-011 Phase 3c.iv (ncc test stub buffer cached_hash):
    // when a b"..." literal is used as a dict key, ncc emits
    // `arg cached_hash_lo int <val>` / `arg cached_hash_hi int <val>`
    // alongside the buffer's raw payload (see ncc's
    // `build_buffer_literal_helper_request`).  The test stub now
    // captures these so the emitted `n00b_buffer_t` object
    // descriptor's `.cached_hash` slot is populated to match the
    // production `n00b_buffer_static_init` helper.  Both halves
    // default to zero for non-dict-key buffer literals.
    uint64_t     cached_hash_lo;
    uint64_t     cached_hash_hi;
} request_t;

static char *
read_stdin(void)
{
    size_t cap = 4096;
    size_t len = 0;
    char  *buf = malloc(cap);
    int    c;

    if (!buf) {
        return NULL;
    }

    while ((c = getchar()) != EOF) {
        if (len + 1 >= cap) {
            cap *= 2;
            char *new_buf = realloc(buf, cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
        }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    return buf;
}

static int
hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static char *
hex_decode_string(const char *hex)
{
    size_t len = strlen(hex);
    if ((len % 2) != 0) {
        return NULL;
    }

    char *out = calloc(len / 2 + 1, 1);
    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < len; i += 2) {
        int hi = hex_value(hex[i]);
        int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            free(out);
            return NULL;
        }
        out[i / 2] = (char)((hi << 4) | lo);
    }
    return out;
}

static int
parse_request(char *input, request_t *req)
{
    char *save = NULL;
    for (char *line = ncc_strtok_r(input, "\n", &save);
         line;
         line = ncc_strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "type_hex ", 9) == 0) {
            req->type = hex_decode_string(line + 9);
        }
        else if (strncmp(line, "type_hash ", 10) == 0) {
            req->type_hash = strdup(line + 10);
        }
        else if (strncmp(line, "prefix ", 7) == 0) {
            req->prefix = strdup(line + 7);
        }
        else if (strncmp(line, "entry_attr_hex ", 15) == 0) {
            req->entry_attr = hex_decode_string(line + 15);
        }
        else if (strncmp(line, "container_kind ", 15) == 0) {
            req->container_kind = strdup(line + 15);
        }
        else if (strncmp(line, "container_target ", 17) == 0) {
            req->container_target = strdup(line + 17);
        }
        else if (strncmp(line, "element_type_hex ", 17) == 0) {
            req->element_type = hex_decode_string(line + 17);
        }
        else if (strncmp(line, "element_type_hash ", 18) == 0) {
            req->element_type_hash = strdup(line + 18);
        }
        else if (strncmp(line, "data_type_hash ", 15) == 0) {
            req->data_type_hash = strdup(line + 15);
        }
        else if (strncmp(line, "readonly ", 9) == 0) {
            req->readonly = atoi(line + 9);
        }
        else if (strncmp(line, "len ", 4) == 0) {
            req->len = (size_t)strtoull(line + 4, NULL, 0);
        }
        else if (strncmp(line, "cap ", 4) == 0) {
            req->cap = (size_t)strtoull(line + 4, NULL, 0);
        }
        else if (strncmp(line, "element_scan_kind ", 18) == 0) {
            req->element_scan_kind = strdup(line + 18);
        }
        else if (strncmp(line, "element_scan_cb_hex ", 20) == 0) {
            req->element_scan_cb = hex_decode_string(line + 20);
        }
        else if (strncmp(line, "element_scan_user_hex ", 22) == 0) {
            req->element_scan_user = hex_decode_string(line + 22);
        }
        else if (strncmp(line, "element_shape_decl_hex ", 23) == 0) {
            req->element_shape_decl = hex_decode_string(line + 23);
        }
        else if (strncmp(line, "identity_namespace_hex ", 23) == 0) {
            req->identity_namespace = hex_decode_string(line + 23);
        }
        else if (strncmp(line, "identity_object_key_hex ", 24) == 0) {
            req->identity_object_key = hex_decode_string(line + 24);
        }
        else if (strncmp(line, "identity_payload_key_hex ", 25) == 0) {
            req->identity_payload_key = hex_decode_string(line + 25);
        }
        else if (strncmp(line, "key_type_hex ", 13) == 0) {
            req->key_type = hex_decode_string(line + 13);
        }
        else if (strncmp(line, "value_type_hex ", 15) == 0) {
            req->value_type = hex_decode_string(line + 15);
        }
        else if (strncmp(line, "key_scan_kind ", 14) == 0) {
            req->key_scan_kind = strdup(line + 14);
        }
        else if (strncmp(line, "value_scan_kind ", 16) == 0) {
            req->value_scan_kind = strdup(line + 16);
        }
        else if (strncmp(line, "key_scan_cb_hex ", 16) == 0) {
            req->key_scan_cb = hex_decode_string(line + 16);
        }
        else if (strncmp(line, "value_scan_cb_hex ", 18) == 0) {
            req->value_scan_cb = hex_decode_string(line + 18);
        }
        else if (strncmp(line, "key_scan_user_hex ", 18) == 0) {
            req->key_scan_user = hex_decode_string(line + 18);
        }
        else if (strncmp(line, "value_scan_user_hex ", 20) == 0) {
            req->value_scan_user = hex_decode_string(line + 20);
        }
        else if (strcmp(line, "key_shape_decl_hex") == 0) {
            req->key_shape_decl = strdup("");
        }
        else if (strncmp(line, "key_shape_decl_hex ", 19) == 0) {
            req->key_shape_decl = hex_decode_string(line + 19);
        }
        else if (strcmp(line, "value_shape_decl_hex") == 0) {
            req->value_shape_decl = strdup("");
        }
        else if (strncmp(line, "value_shape_decl_hex ", 21) == 0) {
            req->value_shape_decl = hex_decode_string(line + 21);
        }
        else if (strncmp(line, "skip_obj_hash ", 14) == 0) {
            req->skip_obj_hash = atoi(line + 14);
        }
        else if (strncmp(line, "cached_hash_emit ", 17) == 0) {
            /* ignored by test helper: scalar-key fixtures always use no */
            (void)line;
        }
        else if (strncmp(line, "arg ", 4) == 0 && !req->payload_hex) {
            char name[128];
            char kind[32];
            size_t len = 0;
            char *hex = NULL;
            /* Dict-pair record:
             *   arg <name|-> pair cinit <key_len> <key_hex> \
             *       <val_len> <val_hex> hash <lo> <hi>
             */
            if (sscanf(line, "arg %127s pair cinit %zu", name, &len) == 2) {
                /* Walk past "arg <name> pair cinit <key_len>" tokens. */
                const char *p = line;
                int         tokens = 0;
                while (*p && tokens < 5) {
                    while (*p && !isspace((unsigned char)*p)) {
                        p++;
                    }
                    tokens++;
                    while (*p && isspace((unsigned char)*p)) {
                        p++;
                    }
                }
                /* p now points at the key_hex token. */
                char key_hex[8192] = {0};
                int  consumed = 0;
                if (sscanf(p, "%8191s%n", key_hex, &consumed) != 1) {
                    fprintf(stderr, "bad dict pair key_hex\n");
                    return 0;
                }
                p += consumed;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                size_t key_len = len;
                char *key_expr = hex_decode_string(key_hex);
                if (!key_expr || strlen(key_expr) != key_len) {
                    free(key_expr);
                    fprintf(stderr, "bad dict pair key_hex decode\n");
                    return 0;
                }

                size_t val_len = 0;
                if (sscanf(p, "%zu%n", &val_len, &consumed) != 1) {
                    free(key_expr);
                    fprintf(stderr, "bad dict pair val_len\n");
                    return 0;
                }
                p += consumed;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                char val_hex[8192] = {0};
                if (sscanf(p, "%8191s%n", val_hex, &consumed) != 1) {
                    free(key_expr);
                    fprintf(stderr, "bad dict pair val_hex\n");
                    return 0;
                }
                p += consumed;
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                char *val_expr = hex_decode_string(val_hex);
                if (!val_expr || strlen(val_expr) != val_len) {
                    free(key_expr);
                    free(val_expr);
                    fprintf(stderr, "bad dict pair val_hex decode\n");
                    return 0;
                }

                uint64_t hash_lo = 0;
                uint64_t hash_hi = 0;
                if (strncmp(p, "hash ", 5) == 0) {
                    p += 5;
                    if (sscanf(p, "%llu %llu",
                               (unsigned long long *)&hash_lo,
                               (unsigned long long *)&hash_hi) != 2) {
                        free(key_expr);
                        free(val_expr);
                        fprintf(stderr, "bad dict pair hash\n");
                        return 0;
                    }
                }
                else {
                    free(key_expr);
                    free(val_expr);
                    fprintf(stderr, "dict pair missing precomputed hash\n");
                    return 0;
                }

                if (req->dict_pair_count == req->dict_pair_cap) {
                    size_t new_cap = req->dict_pair_cap
                                         ? req->dict_pair_cap * 2 : 8;
                    dict_pair_t *items = realloc(req->dict_pairs,
                                                  new_cap * sizeof(*items));
                    if (!items) {
                        free(key_expr);
                        free(val_expr);
                        return 0;
                    }
                    req->dict_pairs = items;
                    req->dict_pair_cap = new_cap;
                }
                req->dict_pairs[req->dict_pair_count++] = (dict_pair_t){
                    .key_expr   = key_expr,
                    .value_expr = val_expr,
                    .hash_lo    = hash_lo,
                    .hash_hi    = hash_hi,
                };
                continue;
            }
            // WP-011 Phase 3c.iv / 5f: capture `arg cached_hash_lo int
            // <val>` / `arg cached_hash_hi int <val>` so the emitted
            // buffer object descriptor's `.cached_hash` slot matches
            // the production `n00b_buffer_static_init` helper.  Phase
            // 5f extends this from the dict-key-only path to every
            // buffer literal emission (standalone declarations and
            // list/array/struct elements via
            // `lower_buffer_literal_ref`); empty buffer literals
            // continue to omit the kwargs and both halves stay zero
            // (algorithm parity with `n00b_buffer_hash`'s empty-input
            // `n00b_hash_word(0ULL)` fallback).
            long long int_val = 0;
            if (sscanf(line, "arg %127s int %lld", name, &int_val) == 2) {
                if (strcmp(name, "cached_hash_lo") == 0) {
                    req->cached_hash_lo = (uint64_t)int_val;
                }
                else if (strcmp(name, "cached_hash_hi") == 0) {
                    req->cached_hash_hi = (uint64_t)int_val;
                }
                // Unknown int args are silently ignored to match the
                // stub's pre-3c.iv tolerance of unrecognized lines.
                continue;
            }
            if (sscanf(line, "arg %127s %31s %zu", name, kind, &len) == 3
                && strcmp(kind, "cinit") == 0) {
                char *p = line;
                for (int spaces = 0; *p && spaces < 4; p++) {
                    if (isspace((unsigned char)*p)) {
                        while (isspace((unsigned char)*p)) {
                            p++;
                        }
                        spaces++;
                        p--;
                    }
                }
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                char *expr = hex_decode_string(p);
                if (!expr) {
                    fprintf(stderr, "bad cinit argument\n");
                    return 0;
                }
                if (req->cinit_len == req->cinit_cap) {
                    size_t new_cap = req->cinit_cap ? req->cinit_cap * 2 : 8;
                    char **items = realloc(req->cinit_items,
                                           new_cap * sizeof(*items));
                    if (!items) {
                        free(expr);
                        return 0;
                    }
                    req->cinit_items = items;
                    req->cinit_cap = new_cap;
                }
                req->cinit_items[req->cinit_len++] = expr;
            }
            else if (sscanf(line, "arg %127s %31s %zu", name, kind, &len) == 3
                     && strcmp(kind, "bytes") == 0) {
                if (strcmp(name, "-") != 0 && strcmp(name, "raw") != 0) {
                    fprintf(stderr, "unexpected static image argument '%s'\n",
                            name);
                    return 0;
                }
                char *p = line;
                for (int spaces = 0; *p && spaces < 4; p++) {
                    if (isspace((unsigned char)*p)) {
                        while (isspace((unsigned char)*p)) {
                            p++;
                        }
                        spaces++;
                        p--;
                    }
                }
                while (*p && isspace((unsigned char)*p)) {
                    p++;
                }
                hex = strdup(p);
                req->payload_hex = hex;
                req->payload_len = len;
            }
        }
    }

    return req->type && req->prefix;
}

static void
emit_payload_bytes(const char *hex)
{
    size_t len = strlen(hex);
    for (size_t i = 0; i < len; i += 2) {
        if (i > 0) {
            putchar(',');
        }
        printf("0x%c%c", hex[i], hex[i + 1]);
    }
}

static size_t
buffer_capacity(size_t len)
{
    if (len == 0) {
        return 16;
    }

    size_t cap = 1;
    while (cap < len) {
        cap <<= 1;
    }
    return cap;
}

static int
has_identity(const request_t *req)
{
    return req->identity_namespace && req->identity_namespace[0]
        && req->identity_object_key && req->identity_object_key[0]
        && req->identity_payload_key && req->identity_payload_key[0];
}

static void
emit_c_string_literal(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*p >= 0x20 && *p < 0x7f) {
                putchar((char)*p);
            }
            else {
                printf("\\%03o", (unsigned)*p);
            }
            break;
        }
    }
    putchar('"');
}

static void
emit_identity_decls(const request_t *req)
{
    if (!has_identity(req)) {
        return;
    }

    printf("static const n00b_static_identity_t %s_payload_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_PAYLOAD,"
           ".namespace_id=",
           req->prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf("};");

    printf("static const n00b_static_identity_t %s_obj_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_STATIC_IMAGE_OBJECT,"
           ".namespace_id=",
           req->prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_object_key);
    printf("};");
}

static void
emit_request_identity_fields(const request_t *req)
{
    if (!has_identity(req)) {
        printf(".identity_namespace=0,.identity_object_key=0,"
               ".identity_payload_key=0,");
        return;
    }

    printf(".identity_namespace=");
    emit_c_string_literal(req->identity_namespace);
    printf(",.identity_object_key=");
    emit_c_string_literal(req->identity_object_key);
    printf(",.identity_payload_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf(",");
}

static int
emit_buffer_image(const request_t *req)
{
    if (!req->readonly) {
        fprintf(stderr, "n00b_buffer_t static images are readonly only\n");
        return 5;
    }
    if (!req->payload_hex) {
        fprintf(stderr, "n00b_buffer_t requires a string payload\n");
        return 4;
    }

    size_t alloc_len = buffer_capacity(req->payload_len);

    printf("NCC_STATIC_INIT_OK &%s_obj\n", req->prefix);
    emit_identity_decls(req);
    printf("static const unsigned char %s_payload[]={", req->prefix);
    emit_payload_bytes(req->payload_hex);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_payload_desc={"
           ".start=(const void*)%s_payload,"
           ".len=(uint64_t)sizeof(%s_payload),"
           ".tinfo=0,.scan_kind=1,.scan_cb=0,.scan_user=0,"
           ".object_id=11,.file=__FILE__,.identity=",
           req->prefix, req->prefix, req->prefix);
    if (has_identity(req)) {
        printf("&%s_payload_id", req->prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=1};");
    printf("static const n00b_static_object_desc_t * const "
           "%s_payload_entry=&%s_payload_desc;",
           req->prefix, req->prefix);
    printf("static const n00b_static_image_request_t %s_request={"
           ".version=1u,.type_hash=0,"
           ".payload_kind=N00B_STATIC_IMAGE_PAYLOAD_BYTES,"
           ".payload=(const void*)%s_payload,"
           ".payload_len=(uint64_t)%zu,"
           ".target_abi={.version=1u,.pointer_bytes=(uint8_t)sizeof(void*),"
           ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,.endian=1u},"
           ".object_flags=1,.required_scan_kind=4,",
           req->prefix, req->prefix, req->payload_len);
    emit_request_identity_fields(req);
    printf("};");
    printf("static const uint64_t %s_offsets[]={"
           "__builtin_offsetof(n00b_buffer_t,data)/sizeof(void*)};",
           req->prefix);
    printf("static n00b_gc_struct_layout_t %s_shape={"
           ".stride=(sizeof(n00b_buffer_t)/sizeof(void*)),"
           ".count=1,.offset_count=1,.offsets=%s_offsets};",
           req->prefix, req->prefix);
    printf("static const n00b_buffer_t %s_obj={"
           ".data=(char*)%s_payload,"
           ".byte_len=(size_t)%zu,"
           ".alloc_len=(size_t)%zu,"
           ".lock=0,.allocator=0,.flags=2,"
           ".scan_kind=N00B_GC_SCAN_KIND_NONE,.scan_cb=0,.scan_user=0};",
           req->prefix, req->prefix, req->payload_len, alloc_len);
    printf("static const n00b_static_object_desc_t %s_obj_desc={"
           ".start=(const void*)&%s_obj,"
           ".len=(uint64_t)sizeof(%s_obj),"
           ".tinfo=0,.scan_kind=4,.scan_cb=n00b_gc_scan_cb_struct_layout,"
           ".scan_user=&%s_shape,.object_id=12,.file=__FILE__,.identity=",
           req->prefix, req->prefix, req->prefix, req->prefix);
    if (has_identity(req)) {
        printf("&%s_obj_id", req->prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=1");
    // WP-011 Phase 3c.iv: mirror the production
    // `n00b_buffer_static_init` helper by emitting `.cached_hash` as a
    // 128-bit literal assembled from the high/low halves ncc threads
    // through for dict-key buffer literals.  The production helper
    // always emits the field (even when both halves are zero); we
    // emit it conditionally so test fixtures whose stripped-down
    // `n00b_static_object_desc_t` lacks the `.cached_hash` field stay
    // compatible (the dict-key buffer case is exercised by future
    // ncc-only tests whose struct definitions include the field).
    if (req->cached_hash_lo != 0 || req->cached_hash_hi != 0) {
        printf(",.cached_hash=(((n00b_uint128_t)0x%016llxULL<<64)"
               "|(n00b_uint128_t)0x%016llxULL)",
               (unsigned long long)req->cached_hash_hi,
               (unsigned long long)req->cached_hash_lo);
    }
    printf("};");
    printf("static const n00b_static_object_desc_t * const "
           "%s_obj_entry=&%s_obj_desc;",
           req->prefix, req->prefix);
    printf("static const n00b_static_image_dependency_t %s_deps[]={"
           "{.desc=&%s_payload_desc,"
           ".relocation_offset=__builtin_offsetof(n00b_buffer_t,data),"
           ".role=\"payload\"}};",
           req->prefix, req->prefix);
    printf("static const n00b_static_image_response_t %s_response "
           "__attribute__((used))={"
           ".version=1u,.request=&%s_request,"
           ".object_start=(const void*)&%s_obj,"
           ".object_len=(uint64_t)sizeof(%s_obj),"
           ".scan_kind=4,.scan_cb=n00b_gc_scan_cb_struct_layout,"
           ".scan_user=&%s_shape,.dependencies=%s_deps,"
           ".dependency_count=1};",
           req->prefix, req->prefix, req->prefix, req->prefix, req->prefix,
           req->prefix);
    return 0;
}

static const char *
or_default(const char *s, const char *fallback)
{
    return s && *s ? s : fallback;
}

static unsigned long long
helper_hash_cstr(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void
emit_cinit_items(const request_t *req)
{
    for (size_t i = 0; i < req->cinit_len; i++) {
        if (i > 0) {
            putchar(',');
        }
        fputs(req->cinit_items[i], stdout);
    }
    if (req->cinit_len == 0) {
        fputs("0", stdout);
    }
}

static int
emit_list_image(const request_t *req)
{
    if (!req->element_type || !req->data_type_hash || req->cap < req->len
        || req->cinit_len != req->len) {
        fprintf(stderr, "bad n00b list static initializer request\n");
        return 6;
    }

    int pointer_target = req->container_target
                      && strcmp(req->container_target, "pointer") == 0;
    const char *flags = req->readonly ? "N00B_STATIC_OBJECT_F_READONLY"
                                      : "N00B_STATIC_OBJECT_F_MUTABLE";
    const char *obj_const = req->readonly ? "const " : "";
    const char *scan_kind = or_default(req->element_scan_kind,
                                       "N00B_GC_SCAN_KIND_NONE");
    const char *scan_cb = or_default(req->element_scan_cb, "nullptr");
    const char *scan_user = or_default(req->element_scan_user, "nullptr");
    const char *type_hash = or_default(req->type_hash, "0");
    unsigned long long data_id = helper_hash_cstr(req->prefix)
                               ^ 0x6c69737464617461ULL;
    unsigned long long lock_id = helper_hash_cstr(req->prefix)
                               ^ 0x6c6973746c6f636bULL;
    unsigned long long obj_id = helper_hash_cstr(req->prefix)
                              ^ 0x6c6973746f626aULL;

    printf("NCC_STATIC_INIT_OK ");
    if (pointer_target) {
        printf("&%s_obj\n", req->prefix);
    }
    else {
        printf("{.data=%s_data,.len=(size_t)%zu,.cap=(size_t)%zu,"
               ".lock=&%s_lock,.allocator=nullptr,.scan_kind=%s,"
               ".scan_cb=%s,.scan_user=%s}\n",
               req->prefix, req->len, req->cap, req->prefix, scan_kind,
               scan_cb, scan_user);
    }

    emit_identity_decls(req);
    printf("%s", req->element_shape_decl ? req->element_shape_decl : "");
    printf("static %s %s_data[%zu]={", req->element_type, req->prefix,
           req->cap ? req->cap : 1);
    emit_cinit_items(req);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_data_desc={"
           ".start=(const void*)%s_data,"
           ".len=(uint64_t)sizeof(%s_data),"
           ".tinfo=%s,.scan_kind=%s,.scan_cb=%s,.scan_user=%s,"
           ".object_id=%lluULL,.file=__FILE__,.identity=",
           req->prefix, req->prefix, req->prefix, req->data_type_hash,
           scan_kind, scan_cb, scan_user, data_id);
    if (has_identity(req)) {
        printf("&%s_payload_id", req->prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=%s};", flags);
    printf("static const n00b_static_object_desc_t * const "
           "%s_data_entry=&%s_data_desc;",
           req->prefix, req->prefix);

    printf("static n00b_rwlock_t %s_lock={0};", req->prefix);
    printf("static const n00b_static_object_desc_t %s_lock_desc={"
           ".start=(const void*)&%s_lock,"
           ".len=(uint64_t)sizeof(%s_lock),"
           ".tinfo=0,.scan_kind=N00B_GC_SCAN_KIND_NONE,"
           ".scan_cb=nullptr,.scan_user=nullptr,"
           ".object_id=%lluULL,.file=__FILE__,.identity=0,"
           ".flags=N00B_STATIC_OBJECT_F_MUTABLE};",
           req->prefix, req->prefix, req->prefix, lock_id);
    printf("static const n00b_static_object_desc_t * const "
           "%s_lock_entry=&%s_lock_desc;",
           req->prefix, req->prefix);

    if (pointer_target) {
        printf("_Static_assert((__builtin_offsetof(%s,data)%%sizeof(void*))==0,"
               "\"list data pointer must be pointer-aligned\");",
               req->type);
        printf("static const uint64_t %s_obj_offsets[]={"
               "__builtin_offsetof(%s,data)/sizeof(void*)};",
               req->prefix, req->type);
        printf("static n00b_gc_struct_layout_t %s_obj_shape={"
               ".stride=(sizeof(%s)/sizeof(void*)),.count=1,"
               ".offset_count=1,.offsets=%s_obj_offsets};",
               req->prefix, req->type, req->prefix);
        printf("static %s%s %s_obj={"
               ".data=%s_data,.len=(size_t)%zu,.cap=(size_t)%zu,"
               ".lock=&%s_lock,.allocator=nullptr,.scan_kind=%s,"
               ".scan_cb=%s,.scan_user=%s};",
               obj_const, req->type, req->prefix, req->prefix, req->len,
               req->cap, req->prefix, scan_kind, scan_cb, scan_user);
        printf("static const n00b_static_object_desc_t %s_obj_desc={"
               ".start=(const void*)&%s_obj,"
               ".len=(uint64_t)sizeof(%s_obj),"
               ".tinfo=%s,.scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
               ".scan_cb=n00b_gc_scan_cb_struct_layout,"
               ".scan_user=&%s_obj_shape,"
               ".object_id=%lluULL,.file=__FILE__,.identity=",
               req->prefix, req->prefix, req->prefix, type_hash,
               req->prefix, obj_id);
        if (has_identity(req)) {
            printf("&%s_obj_id", req->prefix);
        }
        else {
            printf("0");
        }
        printf(",.flags=%s};", flags);
        printf("static const n00b_static_object_desc_t * const "
               "%s_obj_entry=&%s_obj_desc;",
               req->prefix, req->prefix);
    }

    printf("static const n00b_static_image_request_t %s_request={"
           ".version=1u,.type_hash=%s,.payload_kind=N00B_STATIC_IMAGE_PAYLOAD_NONE,"
           ".payload=0,.payload_len=0,.args=0,.arg_count=0,"
           ".target_abi={.version=1u,.pointer_bytes=(uint8_t)sizeof(void*),"
           ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,.endian=1u},"
           ".object_flags=%s,.required_scan_kind=N00B_GC_SCAN_KIND_CALLBACK,",
           req->prefix, type_hash, flags);
    emit_request_identity_fields(req);
    printf("};");
    printf("static const n00b_static_image_dependency_t %s_deps[]={"
           "{.desc=&%s_data_desc,.relocation_offset=0,.role=\"data\"},",
           req->prefix, req->prefix);
    if (pointer_target) {
        printf("{.desc=&%s_lock_desc,.relocation_offset=__builtin_offsetof(%s,lock),"
               ".role=\"lock\"}};",
               req->prefix, req->type);
    }
    else {
        printf("{.desc=&%s_lock_desc,.relocation_offset=0,.role=\"lock\"}};",
               req->prefix);
    }
    if (pointer_target) {
        printf("static const n00b_static_image_response_t %s_response "
               "__attribute__((used))={"
               ".version=1u,.request=&%s_request,"
               ".object_start=(const void*)&%s_obj,"
               ".object_len=(uint64_t)sizeof(%s_obj),"
               ".scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
               ".scan_cb=n00b_gc_scan_cb_struct_layout,"
               ".scan_user=&%s_obj_shape,.dependencies=%s_deps,"
               ".dependency_count=2};",
               req->prefix, req->prefix, req->prefix, req->prefix,
               req->prefix, req->prefix);
    }
    else {
        printf("static const n00b_static_image_response_t %s_response "
               "__attribute__((used))={"
               ".version=1u,.request=&%s_request,"
               ".object_start=0,.object_len=0,"
               ".scan_kind=N00B_GC_SCAN_KIND_NONE,.scan_cb=nullptr,"
               ".scan_user=nullptr,.dependencies=%s_deps,"
               ".dependency_count=2};",
               req->prefix, req->prefix, req->prefix);
    }
    return 0;
}

static void
emit_array_identity_decl(const request_t *req)
{
    if (!has_identity(req)) {
        return;
    }

    printf("static const n00b_static_identity_t %s_data_id={"
           ".version=1u,"
           ".kind=N00B_STATIC_IDENTITY_NCC_ARRAY_DATA,"
           ".namespace_id=",
           req->prefix);
    emit_c_string_literal(req->identity_namespace);
    printf(",.object_key=");
    emit_c_string_literal(req->identity_payload_key);
    printf("};");
}

static int
emit_array_image(const request_t *req)
{
    if (!req->element_type || !req->data_type_hash || req->cap != req->len
        || req->cinit_len != req->len) {
        fprintf(stderr, "bad n00b array static initializer request\n");
        return 6;
    }

    const char *scan_kind = or_default(req->element_scan_kind, "1");
    const char *scan_cb = or_default(req->element_scan_cb, "0");
    const char *scan_user = or_default(req->element_scan_user, "0");
    const char *entry_attr = req->entry_attr ? req->entry_attr : "";
    unsigned long long data_id = helper_hash_cstr(req->prefix)
                               ^ 0x6172726179646174ULL;

    printf("NCC_STATIC_INIT_OK %s_data\n", req->prefix);
    emit_array_identity_decl(req);
    printf("%s", req->element_shape_decl ? req->element_shape_decl : "");
    printf("static %s %s_data[%zu]={", req->element_type, req->prefix,
           req->len ? req->len : 1);
    emit_cinit_items(req);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_data_desc={"
           ".start=(const void*)%s_data,"
           ".len=(uint64_t)sizeof(%s_data),"
           ".tinfo=%s,.scan_kind=%s,.scan_cb=%s,.scan_user=%s,"
           ".object_id=%lluULL,.file=__FILE__,.identity=",
           req->prefix, req->prefix, req->prefix, req->data_type_hash,
           scan_kind, scan_cb, scan_user, data_id);
    if (has_identity(req)) {
        printf("&%s_data_id", req->prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=2};");
    printf("static const n00b_static_object_desc_t * const "
           "%s_data_entry %s=&%s_data_desc;",
           req->prefix, entry_attr, req->prefix);
    return 0;
}

static uint64_t
pow2_ceil_size(uint64_t v)
{
    if (v <= 1) return 1;
    uint64_t r = 1;
    while (r < v) r <<= 1;
    return r;
}

// WP-011 Phase 3c.i test-helper dict emitter.  Mirrors the minimal
// slice of n00b's emit_dict_image needed for the static-image
// compile-and-run fixture: slot-assign each pair by linear probing
// from `hash & mask`, emit the buckets/keys/values arrays, the
// store, and the dict initializer (value- or pointer-target).
static int
emit_dict_image(const request_t *req)
{
    if (!req->key_type || !req->value_type || !req->data_type_hash) {
        fprintf(stderr, "bad n00b dict static initializer request\n");
        return 6;
    }
    if (req->dict_pair_count != req->len) {
        fprintf(stderr, "dict pair count %zu does not match len %zu\n",
                req->dict_pair_count, req->len);
        return 6;
    }

    size_t entry_count = req->dict_pair_count;
    size_t cap = pow2_ceil_size(entry_count);
    if (cap < 16) cap = 16;
    if (req->cap != 0 && req->cap != cap) {
        fprintf(stderr,
                "dict capacity mismatch: request cap=%zu computed cap=%zu\n",
                req->cap, cap);
        return 6;
    }
    uint64_t mask = (uint64_t)(cap - 1);

    /* Slot-assign each pair by linear probing from hash & mask. */
    int64_t *slot_to_pair = malloc(cap * sizeof(*slot_to_pair));
    if (!slot_to_pair) return 6;
    for (size_t s = 0; s < cap; s++) slot_to_pair[s] = -1;
    for (size_t i = 0; i < entry_count; i++) {
        uint64_t start = req->dict_pairs[i].hash_lo & mask;
        int      placed = 0;
        for (size_t probe = 0; probe < cap; probe++) {
            size_t s = (start + probe) & mask;
            if (slot_to_pair[s] == -1) {
                slot_to_pair[s] = (int64_t)i;
                placed = 1;
                break;
            }
        }
        if (!placed) {
            free(slot_to_pair);
            fprintf(stderr, "dict probe exhausted at pair %zu\n", i);
            return 6;
        }
    }

    int pointer_target = req->container_target
                      && strcmp(req->container_target, "pointer") == 0;
    const char *flags = req->readonly ? "N00B_STATIC_OBJECT_F_READONLY"
                                      : "N00B_STATIC_OBJECT_F_MUTABLE";
    const char *obj_const = req->readonly ? "const " : "";
    const char *key_scan_kind = or_default(req->key_scan_kind, "1");
    const char *key_scan_cb = or_default(req->key_scan_cb, "nullptr");
    const char *key_scan_user = or_default(req->key_scan_user, "nullptr");
    const char *value_scan_kind = or_default(req->value_scan_kind, "1");
    const char *value_scan_cb = or_default(req->value_scan_cb, "nullptr");
    const char *value_scan_user = or_default(req->value_scan_user, "nullptr");
    const char *dict_logical_scan_kind = key_scan_kind;
    if (strcmp(dict_logical_scan_kind, "1") == 0
        || strcmp(dict_logical_scan_kind, "N00B_GC_SCAN_KIND_NONE") == 0) {
        dict_logical_scan_kind = value_scan_kind;
    }
    const char *dict_logical_scan_cb = key_scan_cb;
    const char *dict_logical_scan_user = key_scan_user;

    uint32_t threshold = (uint32_t)(cap - (cap >> 1u) - 1u);
    /* Match the n00b helper's threshold formula (75%): cap - cap/4 - 1 */
    threshold = (uint32_t)(cap - (cap >> 2) - 1u);

    unsigned long long obj_id = helper_hash_cstr(req->prefix)
                              ^ 0x646963746f626aULL;

    printf("NCC_STATIC_INIT_OK ");
    if (pointer_target) {
        printf("&%s_obj\n", req->prefix);
    }
    else {
        printf("{.store=(void *)&%s_store,"
               ".fn=nullptr,.allocator=nullptr,"
               ".insertion_epoch=0,.wait_ct=0,.length=(int64_t)%zuULL,"
               "._migration_state=0,.lock=nullptr,.cache=0,"
               ".skip_obj_hash=%u,"
               ".scan_kind=%s,.scan_cb=%s,.scan_user=%s}\n",
               req->prefix, entry_count,
               req->skip_obj_hash ? 1u : 0u,
               dict_logical_scan_kind, dict_logical_scan_cb,
               dict_logical_scan_user);
    }

    emit_identity_decls(req);
    printf("%s", req->key_shape_decl ? req->key_shape_decl : "");
    printf("%s", req->value_shape_decl ? req->value_shape_decl : "");

    /* Buckets array. */
    printf("static n00b_dict_bucket_t %s_buckets[%zu]={",
           req->prefix, cap);
    for (size_t s = 0; s < cap; s++) {
        if (s) putchar(',');
        int64_t pi = slot_to_pair[s];
        if (pi < 0) {
            printf("{}");
        }
        else {
            uint32_t insert_order = (uint32_t)(pi + 1);
            printf("{.hv=((n00b_uint128_t)0x%016llxULL<<64)"
                   "|(n00b_uint128_t)0x%016llxULL,"
                   ".insert_order=(uint32_t)%uu,.flags=0}",
                   (unsigned long long)req->dict_pairs[pi].hash_hi,
                   (unsigned long long)req->dict_pairs[pi].hash_lo,
                   (unsigned)insert_order);
        }
    }
    printf("};");

    /* Keys array. */
    printf("static %s %s_keys[%zu]={",
           req->key_type, req->prefix, cap);
    for (size_t s = 0; s < cap; s++) {
        if (s) putchar(',');
        int64_t pi = slot_to_pair[s];
        if (pi < 0) {
            printf("{}");
        }
        else {
            fputs(req->dict_pairs[pi].key_expr, stdout);
        }
    }
    printf("};");

    /* Values array. */
    printf("static %s %s_values[%zu]={",
           req->value_type, req->prefix, cap);
    for (size_t s = 0; s < cap; s++) {
        if (s) putchar(',');
        int64_t pi = slot_to_pair[s];
        if (pi < 0) {
            printf("{}");
        }
        else {
            fputs(req->dict_pairs[pi].value_expr, stdout);
        }
    }
    printf("};");

    /* Store struct. */
    printf("static __n00b_internal_type_erased_store_t %s_store={"
           ".last_slot=(uint32_t)%uu,.threshold=(uint32_t)%uu,"
           ".used_count=%uu,"
           ".buckets=%s_buckets,"
           ".keys=(void**)%s_keys,"
           ".values=(void**)%s_values};",
           req->prefix,
           (unsigned)(cap - 1u), (unsigned)threshold,
           (unsigned)entry_count,
           req->prefix, req->prefix, req->prefix);

    if (pointer_target) {
        printf("static %s%s %s_obj={"
               ".store=(void *)&%s_store,"
               ".fn=nullptr,.allocator=nullptr,"
               ".insertion_epoch=0,.wait_ct=0,.length=(int64_t)%zuULL,"
               "._migration_state=0,.lock=nullptr,.cache=0,"
               ".skip_obj_hash=%u,"
               ".scan_kind=%s,.scan_cb=%s,.scan_user=%s};",
               obj_const, req->type, req->prefix, req->prefix,
               entry_count,
               req->skip_obj_hash ? 1u : 0u,
               dict_logical_scan_kind, dict_logical_scan_cb,
               dict_logical_scan_user);
        printf("static const n00b_static_object_desc_t %s_obj_desc "
               "__attribute__((used))={"
               ".start=(const void*)&%s_obj,"
               ".len=(uint64_t)sizeof(%s_obj),"
               ".tinfo=0,.scan_kind=N00B_GC_SCAN_KIND_CALLBACK,"
               ".scan_cb=n00b_gc_scan_cb_struct_layout,"
               ".scan_user=nullptr,"
               ".object_id=%lluULL,.file=__FILE__,.identity=nullptr,"
               ".flags=%s};",
               req->prefix, req->prefix, req->prefix,
               obj_id, flags);
    }

    free(slot_to_pair);
    return 0;
}

static void
free_request(request_t *req)
{
    free(req->type);
    free(req->type_hash);
    free(req->prefix);
    free(req->entry_attr);
    free(req->container_kind);
    free(req->container_target);
    free(req->element_type);
    free(req->element_type_hash);
    free(req->data_type_hash);
    free(req->element_scan_kind);
    free(req->element_scan_cb);
    free(req->element_scan_user);
    free(req->element_shape_decl);
    free(req->identity_namespace);
    free(req->identity_object_key);
    free(req->identity_payload_key);
    free(req->payload_hex);
    for (size_t i = 0; i < req->cinit_len; i++) {
        free(req->cinit_items[i]);
    }
    free(req->cinit_items);
    free(req->key_type);
    free(req->value_type);
    free(req->key_scan_kind);
    free(req->value_scan_kind);
    free(req->key_scan_cb);
    free(req->value_scan_cb);
    free(req->key_scan_user);
    free(req->value_scan_user);
    free(req->key_shape_decl);
    free(req->value_shape_decl);
    for (size_t i = 0; i < req->dict_pair_count; i++) {
        free(req->dict_pairs[i].key_expr);
        free(req->dict_pairs[i].value_expr);
    }
    free(req->dict_pairs);
}

int
main(void)
{
    char *input = read_stdin();
    request_t req = {0};

    if (!input || !parse_request(input, &req)) {
        fprintf(stderr, "bad static-image helper request\n");
        return 2;
    }

    if (req.container_kind && strcmp(req.container_kind, "dict") == 0) {
        int status = emit_dict_image(&req);
        free_request(&req);
        free(input);
        return status;
    }

    if (req.container_kind
        && (strcmp(req.container_kind, "list") == 0
            || strcmp(req.container_kind, "array") == 0)) {
        int status = strcmp(req.container_kind, "array") == 0
                   ? emit_array_image(&req)
                   : emit_list_image(&req);
        free(req.type);
        free(req.type_hash);
        free(req.prefix);
        free(req.entry_attr);
        free(req.container_kind);
        free(req.container_target);
        free(req.element_type);
        free(req.element_type_hash);
        free(req.data_type_hash);
        free(req.element_scan_kind);
        free(req.element_scan_cb);
        free(req.element_scan_user);
        free(req.element_shape_decl);
        free(req.identity_namespace);
        free(req.identity_object_key);
        free(req.identity_payload_key);
        free(req.payload_hex);
        for (size_t i = 0; i < req.cinit_len; i++) {
            free(req.cinit_items[i]);
        }
        free(req.cinit_items);
        free(input);
        return status;
    }

    if (strcmp(req.type, "n00b_buffer_t") == 0) {
        int status = emit_buffer_image(&req);
        free(req.type);
        free(req.type_hash);
        free(req.prefix);
        free(req.entry_attr);
        free(req.identity_namespace);
        free(req.identity_object_key);
        free(req.identity_payload_key);
        free(req.payload_hex);
        free(input);
        return status;
    }

    if (strcmp(req.type, "n00b_static_image_test_t") != 0) {
        fprintf(stderr, "static image literal for '%s' is not supported\n",
                req.type);
        return 3;
    }
    if (!req.payload_hex) {
        fprintf(stderr, "n00b_static_image_test_t requires a string payload\n");
        return 4;
    }

    const char *obj_const = req.readonly ? "const " : "";
    const char *flags     = req.readonly ? "1" : "2";

    printf("NCC_STATIC_INIT_OK &%s_obj\n", req.prefix);
    emit_identity_decls(&req);
    printf("static const unsigned char %s_payload[]={", req.prefix);
    emit_payload_bytes(req.payload_hex);
    printf("};");
    printf("static const n00b_static_object_desc_t %s_payload_desc={"
           ".start=(const void*)%s_payload,"
           ".len=(uint64_t)sizeof(%s_payload),"
           ".tinfo=0,.scan_kind=1,.scan_cb=0,.scan_user=0,"
           ".object_id=1,.file=__FILE__,.identity=",
           req.prefix, req.prefix, req.prefix);
    if (has_identity(&req)) {
        printf("&%s_payload_id", req.prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=1};");
    printf("static const n00b_static_object_desc_t * const "
           "%s_payload_entry=&%s_payload_desc;",
           req.prefix, req.prefix);
    printf("static const n00b_static_image_request_t %s_request={"
           ".version=1u,.type_hash=0,"
           ".payload_kind=N00B_STATIC_IMAGE_PAYLOAD_BYTES,"
           ".payload=(const void*)%s_payload,"
           ".payload_len=(uint64_t)%zu,"
           ".target_abi={.version=1u,.pointer_bytes=(uint8_t)sizeof(void*),"
           ".size_t_bytes=(uint8_t)sizeof(size_t),.char_bits=8,.endian=1u},"
           ".object_flags=%s,.required_scan_kind=4,",
           req.prefix, req.prefix, req.payload_len, flags);
    emit_request_identity_fields(&req);
    printf("};");
    printf("static const uint64_t %s_offsets[]={"
           "__builtin_offsetof(n00b_static_image_test_t,bytes)/sizeof(void*)};",
           req.prefix);
    printf("static n00b_gc_struct_layout_t %s_shape={"
           ".stride=(sizeof(n00b_static_image_test_t)/sizeof(void*)),"
           ".count=1,.offset_count=1,.offsets=%s_offsets};",
           req.prefix, req.prefix);
    printf("static %s n00b_static_image_test_t %s_obj={"
           ".magic=0x4E30304253494D47ULL,"
           ".byte_len=(uint64_t)%zu,"
           ".bytes=%s_payload,.constructor_cookie=0};",
           obj_const, req.prefix, req.payload_len, req.prefix);
    printf("static const n00b_static_object_desc_t %s_obj_desc={"
           ".start=(const void*)&%s_obj,"
           ".len=(uint64_t)sizeof(%s_obj),"
           ".tinfo=0,.scan_kind=4,.scan_cb=n00b_gc_scan_cb_struct_layout,"
           ".scan_user=&%s_shape,.object_id=2,.file=__FILE__,.identity=",
           req.prefix, req.prefix, req.prefix, req.prefix);
    if (has_identity(&req)) {
        printf("&%s_obj_id", req.prefix);
    }
    else {
        printf("0");
    }
    printf(",.flags=%s};", flags);
    printf("static const n00b_static_object_desc_t * const "
           "%s_obj_entry=&%s_obj_desc;",
           req.prefix, req.prefix);
    printf("static const n00b_static_image_dependency_t %s_deps[]={"
           "{.desc=&%s_payload_desc,"
           ".relocation_offset=__builtin_offsetof(n00b_static_image_test_t,bytes),"
           ".role=\"payload\"}};",
           req.prefix, req.prefix);
    printf("static const n00b_static_image_response_t %s_response "
           "__attribute__((used))={"
           ".version=1u,.request=&%s_request,"
           ".object_start=(const void*)&%s_obj,"
           ".object_len=(uint64_t)sizeof(%s_obj),"
           ".scan_kind=4,.scan_cb=n00b_gc_scan_cb_struct_layout,"
           ".scan_user=&%s_shape,.dependencies=%s_deps,"
           ".dependency_count=1};",
           req.prefix, req.prefix, req.prefix, req.prefix, req.prefix,
           req.prefix);

    free(req.type);
    free(req.type_hash);
    free(req.prefix);
    free(req.entry_attr);
    free(req.identity_namespace);
    free(req.identity_object_key);
    free(req.identity_payload_key);
    free(req.payload_hex);
    free(input);
    return 0;
}
