#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *type;
    char  *prefix;
    char  *identity_namespace;
    char  *identity_object_key;
    char  *identity_payload_key;
    int    readonly;
    char  *payload_hex;
    size_t payload_len;
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
    for (char *line = strtok_r(input, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "type_hex ", 9) == 0) {
            req->type = hex_decode_string(line + 9);
        }
        else if (strncmp(line, "prefix ", 7) == 0) {
            req->prefix = strdup(line + 7);
        }
        else if (strncmp(line, "readonly ", 9) == 0) {
            req->readonly = atoi(line + 9);
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
        else if (strncmp(line, "arg ", 4) == 0 && !req->payload_hex) {
            char name[128];
            char kind[32];
            size_t len = 0;
            char *hex = NULL;
            if (sscanf(line, "arg %127s %31s %zu", name, kind, &len) == 3
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
    printf(",.flags=1};");
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

int
main(void)
{
    char *input = read_stdin();
    request_t req = {0};

    if (!input || !parse_request(input, &req)) {
        fprintf(stderr, "bad static-image helper request\n");
        return 2;
    }

    if (strcmp(req.type, "n00b_buffer_t") == 0) {
        int status = emit_buffer_image(&req);
        free(req.type);
        free(req.prefix);
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
    free(req.prefix);
    free(req.identity_namespace);
    free(req.identity_object_key);
    free(req.identity_payload_key);
    free(req.payload_hex);
    free(input);
    return 0;
}
