#include "parse/comptime_meta.h"

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/platform.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NCC_CT_VAR_FIXED_PAYLOAD_BYTES (8 + 1 + 1 + 2)
#define NCC_CT_STATIC_INIT_FIXED_PAYLOAD_BYTES (8 + 1 + 1 + 1 + 2)
#define NCC_AR_MAGIC "!<arch>\n"
#define NCC_THIN_AR_MAGIC "!<thin>\n"

static void
append_byte(ncc_buffer_t *buf, uint8_t byte)
{
    ncc_buffer_printf(buf, "0x%02x,", byte);
}

static void
append_u16le(ncc_buffer_t *buf, uint16_t value)
{
    append_byte(buf, (uint8_t)(value & 0xffu));
    append_byte(buf, (uint8_t)((value >> 8) & 0xffu));
}

static void
append_u64le(ncc_buffer_t *buf, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        append_byte(buf, (uint8_t)((value >> (i * 8)) & 0xffu));
    }
}

static uint16_t
read_u16le(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint64_t
read_u64le(const unsigned char *p)
{
    uint64_t value = 0;

    for (int i = 0; i < 8; i++) {
        value |= (uint64_t)p[i] << (i * 8);
    }

    return value;
}

static void
set_err(char **err_out, const char *fmt, ...)
{
    if (!err_out) {
        return;
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    va_list       ap;

    va_start(ap, fmt);
    ncc_buffer_vprintf(buf, fmt, ap);
    va_end(ap);

    ncc_free(*err_out);
    *err_out = ncc_buffer_take(buf);
}

static char *
read_binary_file(const char *path, size_t *len_out, char **err_out)
{
    if (len_out) {
        *len_out = 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err_out, "open failed for '%s': %s",
                path ? path : "(null)", strerror(errno));
        return nullptr;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        set_err(err_out, "seek failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    long end = ftell(f);
    if (end < 0) {
        set_err(err_out, "tell failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        set_err(err_out, "seek failed for '%s': %s", path, strerror(errno));
        fclose(f);
        return nullptr;
    }

    char  *data = ncc_alloc_array(char, (size_t)end + 1);
    size_t got  = fread(data, 1, (size_t)end, f);
    int    rc   = fclose(f);

    if (got != (size_t)end || rc != 0) {
        set_err(err_out, "read failed for '%s': %s", path, strerror(errno));
        ncc_free(data);
        return nullptr;
    }

    if (len_out) {
        *len_out = (size_t)end;
    }
    return data;
}

static bool
path_exists(const char *path, bool *exists_out, char **err_out)
{
    if (exists_out) {
        *exists_out = false;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (exists_out) {
            *exists_out = true;
        }
        return true;
    }

    if (errno == ENOENT || errno == ENOTDIR) {
        return true;
    }

    set_err(err_out, "stat failed for '%s': %s",
            path ? path : "(null)", strerror(errno));
    return false;
}

static bool
append_record(ncc_ct_rec_list_t *list, ncc_ct_rec_t rec)
{
    if (!list) {
        return false;
    }

    if (list->n_records == list->records_cap) {
        int new_cap = list->records_cap ? list->records_cap * 2 : 4;
        list->records = ncc_realloc(list->records,
                                    (size_t)new_cap * sizeof(*list->records));
        list->records_cap = new_cap;
    }

    list->records[list->n_records++] = rec;
    return true;
}

static bool
append_aggregate_var(ncc_ct_aggregate_t *agg, const ncc_ct_var_t *var)
{
    if (agg->n_vars == agg->vars_cap) {
        int new_cap = agg->vars_cap ? agg->vars_cap * 2 : 4;
        agg->vars = ncc_realloc(agg->vars,
                                (size_t)new_cap * sizeof(*agg->vars));
        agg->vars_cap = new_cap;
    }

    agg->vars[agg->n_vars] = (ncc_ct_var_t){
        .name     = ncc_string_from_raw(var->name.data,
                                        (int64_t)var->name.u8_bytes),
        .typehash = var->typehash,
        .linkage  = var->linkage,
        .flags    = var->flags,
    };
    agg->n_vars++;
    return true;
}

static bool
append_aggregate_static_init(ncc_ct_aggregate_t *agg,
                             const ncc_ct_static_init_t *si)
{
    if (agg->n_static_inits == agg->static_inits_cap) {
        int new_cap = agg->static_inits_cap ? agg->static_inits_cap * 2 : 4;
        agg->static_inits = ncc_realloc(
            agg->static_inits, (size_t)new_cap * sizeof(*agg->static_inits));
        agg->static_inits_cap = new_cap;
    }

    agg->static_inits[agg->n_static_inits] = (ncc_ct_static_init_t){
        .name       = ncc_string_from_raw(si->name.data,
                                          (int64_t)si->name.u8_bytes),
        .typehash   = si->typehash,
        .kind       = si->kind,
        .flags      = si->flags,
        .degrade_ok = si->degrade_ok,
    };
    agg->n_static_inits++;
    return true;
}

static bool
var_fields_valid(const ncc_ct_var_t *var)
{
    return var && var->linkage <= 1
        && (var->flags & (uint8_t)~NCC_CT_VAR_FLAG_POINTER_ROOT) == 0;
}

static bool
static_init_fields_valid(const ncc_ct_static_init_t *si)
{
    return si && si->name.data
        && (si->kind == NCC_CT_STATIC_INIT_CONST_RO
            || si->kind == NCC_CT_STATIC_INIT_WRITABLE)
        && (si->flags & (uint8_t)~NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT) == 0
        && si->degrade_ok <= 1;
}

static bool
main_flags_valid(uint8_t flags)
{
    return (flags & (uint8_t)~NCC_CT_MAIN_FLAG_OPTIONAL) == 0;
}

static bool
sig_eq(ncc_ct_sig_t a, ncc_ct_sig_t b)
{
    return a.argc == b.argc
        && a.has_argv == b.has_argv
        && a.has_envp == b.has_envp;
}

static bool
section_absent_diagnostic(const ncc_process_result_t *proc)
{
    if (!proc || !proc->stderr_data) {
        return false;
    }

    return strstr(proc->stderr_data, "can't find section")
        || strstr(proc->stderr_data, "cannot find section")
        || strstr(proc->stderr_data, "section not found")
        || strstr(proc->stderr_data, "not found")
        || strstr(proc->stderr_data, "no section named")
        || strstr(proc->stderr_data, "section named");
}

static char *
process_stderr_string(const ncc_process_result_t *proc)
{
    if (!proc || !proc->stderr_data || proc->stderr_len == 0) {
        return ncc_string_from_cstr("").data;
    }

    char *out = ncc_alloc_array(char, proc->stderr_len + 1);
    memcpy(out, proc->stderr_data, proc->stderr_len);
    return out;
}

static bool
try_objcopy_candidate(const char *program)
{
    if (!program || !program[0]) {
        return false;
    }

    const char *argv[] = { program, "--version", nullptr };
    ncc_process_spec_t spec = {
        .program        = program,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok       = launched && proc.exit_code == 0;

    ncc_process_result_free(&proc);
    return ok;
}

static const char *
find_objcopy(void)
{
    const char *env = getenv("NCC_LLVM_OBJCOPY");

    if (try_objcopy_candidate(env)) {
        return env;
    }
    if (try_objcopy_candidate("llvm-objcopy")) {
        return "llvm-objcopy";
    }
    if (try_objcopy_candidate("objcopy")) {
        return "objcopy";
    }

    return nullptr;
}

static const char *
find_ar(void)
{
    const char *env = getenv("NCC_LLVM_AR");

    if (try_objcopy_candidate(env)) {
        return env;
    }
    if (try_objcopy_candidate("llvm-ar")) {
        return "llvm-ar";
    }
    if (try_objcopy_candidate("ar")) {
        return "ar";
    }

    return nullptr;
}

static const char *const ct_section_names[] = {
    NCC_CT_SECTION_MACHO,
    NCC_CT_SECTION_ELF,
    NCC_CT_SECTION_PE,
    NCC_CT_SECTION_MACHO_SECT,
};

static char *
make_objcopy_section_arg(const char *prefix, const char *section,
                         const char *path)
{
    ncc_buffer_t *buf = ncc_buffer_empty();

    ncc_buffer_puts(buf, prefix);
    ncc_buffer_puts(buf, section);
    if (path) {
        ncc_buffer_putc(buf, '=');
        ncc_buffer_puts(buf, path);
    }

    return ncc_buffer_take(buf);
}

static bool
dump_section(const char *objcopy, const char *obj_path, const char *section,
             const char *out_path, bool *absent, char **err_out)
{
    char *dump_arg = make_objcopy_section_arg("--dump-section=", section,
                                             out_path);
    const char *argv[] = {
        objcopy,
        dump_arg,
        obj_path,
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = objcopy,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok       = launched && proc.exit_code == 0;

    if (ok) {
        bool exists = false;
        if (!path_exists(out_path, &exists, err_out)) {
            ok = false;
        }
        else if (!exists) {
            if (absent) {
                *absent = true;
            }
            ncc_process_result_free(&proc);
            ncc_free(dump_arg);
            return false;
        }
    }

    if (absent) {
        *absent = !ok && launched && section_absent_diagnostic(&proc);
    }

    if (!ok && (!absent || !*absent)) {
        char *stderr_text = process_stderr_string(&proc);
        set_err(err_out, "%s --dump-section failed for '%s': %s",
                objcopy, obj_path, stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(dump_arg);
    return ok;
}

// Public: dump `section` from a single object file and return its raw bytes
// (caller frees with ncc_free). A missing section is not an error: sets
// *bytes_out=NULL, *len_out=0, returns true. Reuses find_objcopy/dump_section.
bool
ncc_ct_read_object_section(const char *obj_path,
                           const char *section,
                           uint8_t   **bytes_out,
                           size_t     *len_out,
                           char      **err_out)
{
    *bytes_out = nullptr;
    *len_out   = 0;

    const char *objcopy = find_objcopy();
    if (!objcopy) {
        set_err(err_out, "llvm-objcopy or objcopy is required to read '%s'",
                section);
        return false;
    }

    ncc_temp_workspace_t tmp     = {0};
    char                *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_gcraw_", &tmp_err)) {
        set_err(err_out, "%s",
                tmp_err ? tmp_err : "failed to create gcraw temp workspace");
        ncc_free(tmp_err);
        return false;
    }

    char *out_path = ncc_temp_workspace_join(&tmp, "section.bin");
    bool  absent   = false;
    char *dump_err = nullptr;
    bool  ok = dump_section(objcopy, obj_path, section, out_path, &absent,
                            &dump_err);
    if (!ok) {
        ncc_free(out_path);
        ncc_temp_workspace_cleanup(&tmp);
        if (absent) {
            ncc_free(dump_err);
            return true; // object simply has no such section
        }
        set_err(err_out, "%s", dump_err ? dump_err : "section dump failed");
        ncc_free(dump_err);
        return false;
    }

    size_t n    = 0;
    char  *data = read_binary_file(out_path, &n, err_out);
    ncc_free(out_path);
    ncc_temp_workspace_cleanup(&tmp);
    if (!data) {
        return false;
    }
    *bytes_out = (uint8_t *)data;
    *len_out   = n;
    return true;
}

static char *join_dir_leaf(const char *dir, const char *leaf); // defined below
static bool  list_archive_members(const char *ar, const char *archive_path,
                                  ncc_process_result_t *proc, char **err_out);
static bool  extract_archive_members(const char *ar, const char *archive_path,
                                     const char *out_dir, char **err_out);
static bool  is_thin_archive_bytes(const char *data, size_t len);
static char *resolve_thin_archive_member(const char *archive_path,
                                         const char *member);

// Public: read and concatenate `section` bytes from a link input — either a
// single object (.o/.obj) or every object member of an archive (.a/.lib). The caller
// frees *bytes_out via ncc_free. Inputs that are neither, or that lack the
// section, contribute nothing (returns true, possibly with *len_out unchanged).
// Records are self-delimiting, so concatenation across members is valid.
bool
ncc_ct_read_input_section(const char *input_path,
                          const char *section,
                          uint8_t   **bytes_out,
                          size_t     *len_out,
                          char      **err_out)
{
    *bytes_out = nullptr;
    *len_out   = 0;

    const char *ext = strrchr(input_path, '.');
    if (ext && (strcmp(ext, ".o") == 0 || strcmp(ext, ".obj") == 0)) {
        return ncc_ct_read_object_section(input_path, section, bytes_out,
                                          len_out, err_out);
    }
    if (!ext || (strcmp(ext, ".a") != 0 && strcmp(ext, ".lib") != 0)) {
        return true; // not an object/archive we scan
    }

    // Archive: resolve thin members in place, or extract regular members, then
    // read the requested section from each object.
    const char *ar = find_ar();
    if (!ar) {
        set_err(err_out, "llvm-ar or ar is required to read archive '%s'",
                input_path);
        return false;
    }
    ncc_temp_workspace_t tmp     = {0};
    char                *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_gcraw_ar_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err : "archive temp workspace failed");
        ncc_free(tmp_err);
        return false;
    }
    const char *dir = ncc_temp_workspace_path(&tmp);

    size_t archive_len = 0;
    char  *archive_data = read_binary_file(input_path, &archive_len, err_out);
    if (!archive_data) {
        ncc_temp_workspace_cleanup(&tmp);
        return false;
    }
    bool thin = is_thin_archive_bytes(archive_data, archive_len);
    ncc_free(archive_data);

    ncc_process_result_t list = {0};
    bool ok = list_archive_members(ar, input_path, &list, err_out)
           && (thin || extract_archive_members(ar, input_path, dir, err_out));
    if (!ok) {
        ncc_process_result_free(&list);
        ncc_temp_workspace_cleanup(&tmp);
        return false;
    }

    ncc_buffer_t *acc   = ncc_buffer_empty();
    size_t        total = 0;
    size_t        pos   = 0;
    while (ok && pos < list.stdout_len) {
        size_t start = pos;
        while (pos < list.stdout_len && list.stdout_data[pos] != '\n') {
            pos++;
        }
        size_t mlen = pos - start;
        if (pos < list.stdout_len) {
            pos++; // skip newline
        }
        while (mlen > 0
               && (list.stdout_data[start + mlen - 1] == '\r'
                   || list.stdout_data[start + mlen - 1] == ' ')) {
            mlen--;
        }
        bool object_member = (mlen >= 2
                              && memcmp(list.stdout_data + start + mlen - 2,
                                        ".o", 2) == 0)
                          || (mlen >= 4
                              && memcmp(list.stdout_data + start + mlen - 4,
                                        ".obj", 4) == 0);
        if (!object_member) {
            continue;
        }
        char *name = ncc_alloc_array(char, mlen + 1);
        memcpy(name, list.stdout_data + start, mlen);
        name[mlen] = '\0';
        char *member = thin ? resolve_thin_archive_member(input_path, name)
                            : join_dir_leaf(dir, name);
        ncc_free(name);

        uint8_t *mbytes = nullptr;
        size_t   mlen_b = 0;
        char    *merr   = nullptr;
        if (!ncc_ct_read_object_section(member, section, &mbytes, &mlen_b,
                                        &merr)) {
            set_err(err_out, "gcraw: archive member '%s': %s", member,
                    merr ? merr : "read failed");
            ncc_free(merr);
            ncc_free(member);
            ok = false;
            break;
        }
        if (mbytes && mlen_b) {
            ncc_buffer_append(acc, (const char *)mbytes, mlen_b);
            total += mlen_b;
        }
        ncc_free(mbytes);
        ncc_free(member);
    }

    ncc_process_result_free(&list);
    ncc_temp_workspace_cleanup(&tmp);

    if (!ok) {
        ncc_free(ncc_buffer_take(acc));
        return false;
    }
    if (total == 0) {
        ncc_free(ncc_buffer_take(acc));
        return true;
    }
    *bytes_out = (uint8_t *)ncc_buffer_take(acc);
    *len_out   = total;
    return true;
}

static bool
remove_section(const char *objcopy, const char *in_path, const char *section,
               const char *out_path, bool *absent, char **err_out)
{
    char *remove_arg = make_objcopy_section_arg("--remove-section=", section,
                                               nullptr);
    const char *argv[] = {
        objcopy,
        remove_arg,
        in_path,
        out_path,
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = objcopy,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok       = launched && proc.exit_code == 0;

    if (absent) {
        *absent = !ok && launched && section_absent_diagnostic(&proc);
    }

    if (!ok && (!absent || !*absent)) {
        char *stderr_text = process_stderr_string(&proc);
        set_err(err_out, "%s --remove-section failed for '%s': %s",
                objcopy, in_path, stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    ncc_free(remove_arg);
    return ok;
}

static bool
parse_metadata_bytes(const unsigned char *data, size_t len,
                     ncc_ct_rec_list_t *out, char **err_out)
{
    if (len < 6) {
        set_err(err_out, "comptime metadata section is truncated");
        return false;
    }

    if (memcmp(data, NCC_CT_MAGIC, 4) != 0) {
        set_err(err_out, "comptime metadata magic mismatch");
        return false;
    }

    uint16_t version = read_u16le(data + 4);
    if (version != NCC_CT_FORMAT_VERSION) {
        set_err(err_out, "unsupported comptime metadata version %u",
                (unsigned)version);
        return false;
    }

    size_t pos = 6;
    while (pos < len) {
        if (len - pos < 4) {
            set_err(err_out, "truncated comptime metadata record header");
            return false;
        }

        ncc_ct_rec_kind_t kind = (ncc_ct_rec_kind_t)read_u16le(data + pos);
        uint16_t len16 = read_u16le(data + pos + 2);
        pos += 4;

        if (len - pos < len16) {
            set_err(err_out, "truncated comptime metadata record payload");
            return false;
        }

        const unsigned char *payload = data + pos;

        if (kind == NCC_CT_REC_END) {
            if (len16 != 0) {
                set_err(err_out, "comptime metadata END record has payload");
                return false;
            }
            return true;
        }

        if (kind == NCC_CT_REC_COMPTIME_MAIN) {
            if (len16 != 4) {
                set_err(err_out, "COMPTIME_MAIN record has invalid length %u",
                        (unsigned)len16);
                return false;
            }
            if (!main_flags_valid(payload[3])) {
                set_err(err_out, "COMPTIME_MAIN record has invalid flags 0x%02x",
                        (unsigned)payload[3]);
                return false;
            }

            ncc_ct_rec_t rec = {
                .kind = NCC_CT_REC_COMPTIME_MAIN,
                .sig = {
                    .argc     = payload[0],
                    .has_argv = payload[1] != 0,
                    .has_envp = payload[2] != 0,
                },
                .main_flags = payload[3],
            };
            append_record(out, rec);
        }
        else if (kind == NCC_CT_REC_VAR) {
            if (len16 < NCC_CT_VAR_FIXED_PAYLOAD_BYTES) {
                set_err(err_out, "VAR record has invalid length %u",
                        (unsigned)len16);
                return false;
            }

            uint16_t name_len = read_u16le(payload + 10);
            if (name_len != len16 - NCC_CT_VAR_FIXED_PAYLOAD_BYTES) {
                set_err(err_out, "VAR record name length mismatch");
                return false;
            }

            uint8_t linkage = payload[8];
            uint8_t flags   = payload[9];
            if (linkage > 1) {
                set_err(err_out, "VAR record has invalid linkage %u",
                        (unsigned)linkage);
                return false;
            }
            if ((flags & (uint8_t)~NCC_CT_VAR_FLAG_POINTER_ROOT) != 0) {
                set_err(err_out, "VAR record has invalid flags 0x%02x",
                        (unsigned)flags);
                return false;
            }

            ncc_ct_rec_t rec = {
                .kind = NCC_CT_REC_VAR,
                .var = {
                    .typehash = read_u64le(payload),
                    .linkage  = linkage,
                    .flags    = flags,
                    .name     = ncc_string_from_raw((const char *)payload + 12,
                                                    name_len),
                },
            };
            append_record(out, rec);
        }
        else if (kind == NCC_CT_REC_STATIC_INIT) {
            if (len16 < NCC_CT_STATIC_INIT_FIXED_PAYLOAD_BYTES) {
                set_err(err_out, "STATIC_INIT record has invalid length %u",
                        (unsigned)len16);
                return false;
            }

            uint16_t name_len = read_u16le(payload + 11);
            if (name_len != len16 - NCC_CT_STATIC_INIT_FIXED_PAYLOAD_BYTES) {
                set_err(err_out, "STATIC_INIT record name length mismatch");
                return false;
            }

            uint8_t si_kind = payload[8];
            uint8_t flags = payload[9];
            uint8_t degrade_ok = payload[10];
            if (si_kind != NCC_CT_STATIC_INIT_CONST_RO
                && si_kind != NCC_CT_STATIC_INIT_WRITABLE) {
                set_err(err_out, "STATIC_INIT record has invalid kind %u",
                        (unsigned)si_kind);
                return false;
            }
            if ((flags
                 & (uint8_t)~NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT) != 0) {
                set_err(err_out, "STATIC_INIT record has invalid flags 0x%02x",
                        (unsigned)flags);
                return false;
            }
            if (degrade_ok > 1) {
                set_err(err_out,
                        "STATIC_INIT record has invalid degrade flag %u",
                        (unsigned)degrade_ok);
                return false;
            }

            ncc_ct_rec_t rec = {
                .kind = NCC_CT_REC_STATIC_INIT,
                .static_init = {
                    .typehash = read_u64le(payload),
                    .kind = si_kind,
                    .flags = flags,
                    .degrade_ok = degrade_ok,
                    .name = ncc_string_from_raw((const char *)payload + 13,
                                                name_len),
                },
            };
            append_record(out, rec);
        }

        pos += len16;
    }

    set_err(err_out, "comptime metadata missing END record");
    return false;
}

static void
truncate_record_list(ncc_ct_rec_list_t *list, int n_records)
{
    if (!list || n_records < 0 || n_records > list->n_records) {
        return;
    }

    for (int i = n_records; i < list->n_records; i++) {
        if (list->records[i].kind == NCC_CT_REC_VAR) {
            ncc_free(list->records[i].var.name.data);
        }
        else if (list->records[i].kind == NCC_CT_REC_STATIC_INIT) {
            ncc_free(list->records[i].static_init.name.data);
        }
    }

    list->n_records = n_records;
}

static bool
is_archive_bytes(const char *data, size_t len)
{
    return data
        && ((len >= strlen(NCC_AR_MAGIC)
             && memcmp(data, NCC_AR_MAGIC, strlen(NCC_AR_MAGIC)) == 0)
            || (len >= strlen(NCC_THIN_AR_MAGIC)
                && memcmp(data, NCC_THIN_AR_MAGIC,
                          strlen(NCC_THIN_AR_MAGIC)) == 0));
}

static bool
is_thin_archive_bytes(const char *data, size_t len)
{
    return data && len >= strlen(NCC_THIN_AR_MAGIC)
        && memcmp(data, NCC_THIN_AR_MAGIC, strlen(NCC_THIN_AR_MAGIC)) == 0;
}

static bool
contains_bytes(const char *data, size_t len, const char *needle,
               size_t needle_len)
{
    if (!data || !needle || needle_len == 0 || len < needle_len) {
        return false;
    }

    size_t max = len - needle_len;
    for (size_t i = 0; i <= max; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            return true;
        }
    }

    return false;
}

static bool
list_archive_members(const char *ar, const char *archive_path,
                     ncc_process_result_t *proc, char **err_out)
{
    const char *argv[] = {
        ar,
        "t",
        archive_path,
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = ar,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    bool launched = ncc_process_run(&spec, proc);
    bool ok       = launched && proc->exit_code == 0;

    if (!ok) {
        char *stderr_text = process_stderr_string(proc);
        set_err(err_out, "%s t failed for '%s': %s", ar, archive_path,
                stderr_text);
        ncc_free(stderr_text);
    }

    return ok;
}

static bool
extract_archive_members(const char *ar, const char *archive_path,
                        const char *out_dir, char **err_out)
{
    const char *argv[] = {
        ar,
        "x",
        "--output",
        out_dir,
        archive_path,
        nullptr,
    };
    ncc_process_spec_t spec = {
        .program        = ar,
        .argv           = argv,
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t proc = {0};
    bool launched = ncc_process_run(&spec, &proc);
    bool ok       = launched && proc.exit_code == 0;

    if (!ok) {
        char *stderr_text = process_stderr_string(&proc);
        set_err(err_out, "%s x failed for '%s': %s", ar, archive_path,
                stderr_text);
        ncc_free(stderr_text);
    }

    ncc_process_result_free(&proc);
    return ok;
}

static void
archive_member_line(const ncc_process_result_t *proc, size_t *pos,
                    size_t *start_out, size_t *len_out)
{
    size_t start = *pos;

    while (*pos < proc->stdout_len && proc->stdout_data[*pos] != '\n') {
        (*pos)++;
    }

    size_t len = *pos - start;
    if (len > 0 && proc->stdout_data[start + len - 1] == '\r') {
        len--;
    }

    if (*pos < proc->stdout_len && proc->stdout_data[*pos] == '\n') {
        (*pos)++;
    }

    *start_out = start;
    *len_out   = len;
}

static bool
archive_member_list_has_duplicate(const ncc_process_result_t *proc,
                                  ncc_string_t *dup_out)
{
    if (dup_out) {
        *dup_out = (ncc_string_t){0};
    }

    size_t outer = 0;
    while (outer < proc->stdout_len) {
        size_t a_start = 0;
        size_t a_len   = 0;
        archive_member_line(proc, &outer, &a_start, &a_len);
        if (a_len == 0) {
            continue;
        }

        size_t inner = outer;
        while (inner < proc->stdout_len) {
            size_t b_start = 0;
            size_t b_len   = 0;
            archive_member_line(proc, &inner, &b_start, &b_len);
            if (b_len == a_len
                && memcmp(proc->stdout_data + a_start,
                          proc->stdout_data + b_start, a_len) == 0) {
                if (dup_out) {
                    *dup_out = ncc_string_from_raw(proc->stdout_data + a_start,
                                                  (int64_t)a_len);
                }
                return true;
            }
        }
    }

    return false;
}

static void
append_tlv_header(ncc_buffer_t *buf, ncc_ct_rec_kind_t kind, uint16_t len)
{
    append_u16le(buf, (uint16_t)kind);
    append_u16le(buf, len);
}

static void
append_comptime_main_record(ncc_buffer_t *buf, const ncc_ct_sig_t *sig,
                            uint8_t main_flags)
{
    append_tlv_header(buf, NCC_CT_REC_COMPTIME_MAIN, 4);
    append_byte(buf, sig->argc);
    append_byte(buf, sig->has_argv ? 1 : 0);
    append_byte(buf, sig->has_envp ? 1 : 0);
    append_byte(buf, main_flags);
}

static bool
var_payload_len(const ncc_ct_var_t *var, uint16_t *out_len)
{
    if (!var || !var->name.data || !var_fields_valid(var)) {
        return false;
    }

    if (var->name.u8_bytes
        > UINT16_MAX - NCC_CT_VAR_FIXED_PAYLOAD_BYTES) {
        return false;
    }

    *out_len = (uint16_t)(NCC_CT_VAR_FIXED_PAYLOAD_BYTES
                          + var->name.u8_bytes);
    return true;
}

static bool
static_init_payload_len(const ncc_ct_static_init_t *si, uint16_t *out_len)
{
    if (!si || !si->name.data || !static_init_fields_valid(si)) {
        return false;
    }

    if (si->name.u8_bytes
        > UINT16_MAX - NCC_CT_STATIC_INIT_FIXED_PAYLOAD_BYTES) {
        return false;
    }

    *out_len = (uint16_t)(NCC_CT_STATIC_INIT_FIXED_PAYLOAD_BYTES
                          + si->name.u8_bytes);
    return true;
}

static void
append_var_record(ncc_buffer_t *buf, const ncc_ct_var_t *var, uint16_t payload_len)
{
    append_tlv_header(buf, NCC_CT_REC_VAR, payload_len);
    append_u64le(buf, var->typehash);
    append_byte(buf, var->linkage);
    append_byte(buf, var->flags);
    append_u16le(buf, (uint16_t)var->name.u8_bytes);
    for (uint16_t i = 0; i < var->name.u8_bytes; i++) {
        append_byte(buf, (uint8_t)var->name.data[i]);
    }
}

static void
append_static_init_record(ncc_buffer_t *buf, const ncc_ct_static_init_t *si,
                          uint16_t payload_len)
{
    append_tlv_header(buf, NCC_CT_REC_STATIC_INIT, payload_len);
    append_u64le(buf, si->typehash);
    append_byte(buf, si->kind);
    append_byte(buf, si->flags);
    append_byte(buf, si->degrade_ok);
    append_u16le(buf, (uint16_t)si->name.u8_bytes);
    for (uint16_t i = 0; i < si->name.u8_bytes; i++) {
        append_byte(buf, (uint8_t)si->name.data[i]);
    }
}

const char *
ncc_ct_emit_section_decl_ex(const ncc_opts_t *opts, const ncc_ct_sig_t *sig,
                            uint8_t main_flags, const ncc_ct_var_t *vars,
                            int n_vars,
                            const ncc_ct_static_init_t *static_inits,
                            int n_static_inits)
{
    (void)opts;

    if (!sig && n_vars <= 0 && n_static_inits <= 0) {
        return nullptr;
    }
    if (!main_flags_valid(main_flags)) {
        return nullptr;
    }
    if (!sig && main_flags != 0) {
        return nullptr;
    }

    for (int i = 0; vars && i < n_vars; i++) {
        uint16_t payload_len = 0;
        if (!var_payload_len(&vars[i], &payload_len)) {
            return nullptr;
        }
    }
    for (int i = 0; static_inits && i < n_static_inits; i++) {
        uint16_t payload_len = 0;
        if (!static_init_payload_len(&static_inits[i], &payload_len)) {
            return nullptr;
        }
    }

    ncc_buffer_t *buf = ncc_buffer_empty();

    ncc_buffer_puts(buf,
        "# line 1 \"ncc-generated-comptime-meta.c\"\n"
        "#if defined(__APPLE__)\n"
        "[[gnu::used]] [[gnu::retain]] "
        "[[gnu::section(\"" NCC_CT_SECTION_MACHO "\")]]\n"
        "#elif defined(_WIN32)\n"
        "[[gnu::used]] [[gnu::retain]] "
        "[[gnu::section(\"" NCC_CT_SECTION_PE "\")]]\n"
        "#else\n"
        "[[gnu::used]] [[gnu::retain]] "
        "[[gnu::section(\"" NCC_CT_SECTION_ELF "\")]]\n"
        "#endif\n"
        "static const unsigned char __n00b_ct_meta[] = {\n"
        "0x4e,0x30,0x43,0x54,");

    append_u16le(buf, NCC_CT_FORMAT_VERSION);

    if (sig) {
        append_comptime_main_record(buf, sig, main_flags);
    }

    for (int i = 0; vars && i < n_vars; i++) {
        uint16_t payload_len = 0;
        (void)var_payload_len(&vars[i], &payload_len);
        append_var_record(buf, &vars[i], payload_len);
    }

    for (int i = 0; static_inits && i < n_static_inits; i++) {
        uint16_t payload_len = 0;
        (void)static_init_payload_len(&static_inits[i], &payload_len);
        append_static_init_record(buf, &static_inits[i], payload_len);
    }

    append_tlv_header(buf, NCC_CT_REC_END, 0);

    ncc_buffer_puts(buf,
        "\n};\n"
        "# line 1 \"ncc-generated-comptime-meta-end.c\"\n");

    return ncc_buffer_take(buf);
}

const char *
ncc_ct_emit_section_decl(const ncc_opts_t *opts, const ncc_ct_sig_t *sig,
                         uint8_t main_flags, const ncc_ct_var_t *vars,
                         int n_vars)
{
    return ncc_ct_emit_section_decl_ex(opts, sig, main_flags, vars, n_vars,
                                       nullptr, 0);
}

void
ncc_ct_rec_list_free(ncc_ct_rec_list_t *list)
{
    if (!list) {
        return;
    }

    for (int i = 0; i < list->n_records; i++) {
        if (list->records[i].kind == NCC_CT_REC_VAR) {
            ncc_free(list->records[i].var.name.data);
        }
        else if (list->records[i].kind == NCC_CT_REC_STATIC_INIT) {
            ncc_free(list->records[i].static_init.name.data);
        }
    }

    ncc_free(list->records);
    *list = (ncc_ct_rec_list_t){0};
}

void
ncc_ct_aggregate_free(ncc_ct_aggregate_t *agg)
{
    if (!agg) {
        return;
    }

    for (int i = 0; i < agg->n_vars; i++) {
        ncc_free(agg->vars[i].name.data);
    }
    for (int i = 0; i < agg->n_static_inits; i++) {
        ncc_free(agg->static_inits[i].name.data);
    }

    ncc_free(agg->vars);
    ncc_free(agg->static_inits);
    *agg = (ncc_ct_aggregate_t){0};
}

static bool
path_is_absolute(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }

#ifdef _WIN32
    return path[0] == '/'
        || path[0] == '\\'
        || ((path[0] >= 'A' && path[0] <= 'Z')
            || (path[0] >= 'a' && path[0] <= 'z'))
               && path[1] == ':';
#else
    return path[0] == '/';
#endif
}

static char *
join_dir_leaf(const char *dir, const char *leaf)
{
    if (!dir || !dir[0]) {
        return ncc_string_from_cstr(leaf ? leaf : "").data;
    }

    ncc_buffer_t *buf = ncc_buffer_empty();
    ncc_buffer_puts(buf, dir);
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] != '/') {
        ncc_buffer_putc(buf, '/');
    }
    ncc_buffer_puts(buf, leaf ? leaf : "");
    return ncc_buffer_take(buf);
}

static char *
resolve_thin_archive_member(const char *archive_path, const char *member)
{
    if (path_is_absolute(member)) {
        return ncc_string_from_cstr(member).data;
    }

    bool exists = false;
    if (path_exists(member, &exists, nullptr) && exists) {
        return ncc_string_from_cstr(member).data;
    }

    char *dir = ncc_platform_dirname(archive_path);
    char *out = join_dir_leaf(dir, member);
    ncc_free(dir);
    return out;
}

static bool
read_archive(const ncc_opts_t *opts, const char *archive_path, bool thin,
             ncc_ct_rec_list_t *out, char **err_out)
{
    int base_records = out ? out->n_records : 0;
    int base_scanned = out ? out->n_objects_scanned : 0;

    const char *ar = find_ar();
    if (!ar) {
        set_err(err_out, "llvm-ar or ar is required to read archive '%s'",
                archive_path);
        return false;
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_ct_ar_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err
                                       : "failed to create archive temp workspace");
        ncc_free(tmp_err);
        return false;
    }

    ncc_process_result_t list_proc = {0};
    if (!list_archive_members(ar, archive_path, &list_proc, err_out)) {
        truncate_record_list(out, base_records);
        out->n_objects_scanned = base_scanned;
        ncc_process_result_free(&list_proc);
        ncc_temp_workspace_cleanup(&tmp);
        return false;
    }

    ncc_string_t dup = {0};
    if (archive_member_list_has_duplicate(&list_proc, &dup)) {
        set_err(err_out, "archive '%s' has duplicate member name '%.*s'",
                archive_path, (int)dup.u8_bytes, dup.data);
        ncc_free(dup.data);
        truncate_record_list(out, base_records);
        out->n_objects_scanned = base_scanned;
        ncc_process_result_free(&list_proc);
        ncc_temp_workspace_cleanup(&tmp);
        return false;
    }

    if (!thin) {
        if (!extract_archive_members(ar, archive_path,
                                     ncc_temp_workspace_path(&tmp), err_out)) {
            truncate_record_list(out, base_records);
            out->n_objects_scanned = base_scanned;
            ncc_process_result_free(&list_proc);
            ncc_temp_workspace_cleanup(&tmp);
            return false;
        }
    }

    size_t pos = 0;
    while (pos < list_proc.stdout_len) {
        size_t start = pos;
        while (pos < list_proc.stdout_len
               && list_proc.stdout_data[pos] != '\n') {
            pos++;
        }

        size_t len = pos - start;
        if (len > 0 && list_proc.stdout_data[start + len - 1] == '\r') {
            len--;
        }

        if (len > 0) {
            ncc_string_t member = ncc_string_from_raw(
                list_proc.stdout_data + start, (int64_t)len);
            char *member_path = thin
                ? resolve_thin_archive_member(archive_path, member.data)
                : ncc_temp_workspace_join(&tmp, member.data);
            ncc_free(member.data);

            if (!member_path) {
                set_err(err_out, "failed to resolve archive member path");
                truncate_record_list(out, base_records);
                out->n_objects_scanned = base_scanned;
                ncc_process_result_free(&list_proc);
                ncc_temp_workspace_cleanup(&tmp);
                return false;
            }

            if (!ncc_ct_read_object(opts, member_path, out, err_out)) {
                ncc_free(member_path);
                truncate_record_list(out, base_records);
                out->n_objects_scanned = base_scanned;
                ncc_process_result_free(&list_proc);
                ncc_temp_workspace_cleanup(&tmp);
                return false;
            }

            ncc_free(member_path);
        }

        if (pos < list_proc.stdout_len && list_proc.stdout_data[pos] == '\n') {
            pos++;
        }
    }

    ncc_process_result_free(&list_proc);
    ncc_temp_workspace_cleanup(&tmp);
    return true;
}

bool
ncc_ct_read_object(const ncc_opts_t *opts, const char *obj_path,
                   ncc_ct_rec_list_t *out, char **err_out)
{
    (void)opts;

    if (err_out) {
        *err_out = nullptr;
    }

    if (!obj_path || !out) {
        set_err(err_out, "ncc_ct_read_object requires object path and output");
        return false;
    }

    char *precheck_err = nullptr;
    size_t ignored_len = 0;
    char *ignored = read_binary_file(obj_path, &ignored_len, &precheck_err);
    if (!ignored) {
        set_err(err_out, "%s", precheck_err ? precheck_err
                                            : "object file is unreadable");
        ncc_free(precheck_err);
        return false;
    }
    if (is_archive_bytes(ignored, ignored_len)) {
        bool thin = is_thin_archive_bytes(ignored, ignored_len);
        bool may_have_metadata = contains_bytes(ignored, ignored_len,
                                                NCC_CT_MAGIC,
                                                strlen(NCC_CT_MAGIC));
        ncc_free(ignored);
        if (!thin && !may_have_metadata) {
            return true;
        }
        return read_archive(opts, obj_path, thin, out, err_out);
    }

    if (!contains_bytes(ignored, ignored_len, NCC_CT_MAGIC,
                        strlen(NCC_CT_MAGIC))) {
        ncc_free(ignored);
        out->n_objects_scanned++;
        return true;
    }
    ncc_free(ignored);

    const char *objcopy = find_objcopy();
    if (!objcopy) {
        set_err(err_out, "llvm-objcopy or objcopy is required");
        return false;
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_ct_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err
                                       : "failed to create temp workspace");
        ncc_free(tmp_err);
        return false;
    }

    bool saw_only_absent = true;
    bool ok = false;

    for (size_t i = 0; i < sizeof(ct_section_names) / sizeof(ct_section_names[0]);
         i++) {
        ncc_buffer_t *leaf = ncc_buffer_empty();
        ncc_buffer_printf(leaf, "section-%zu.bin", i);
        char *leaf_text = ncc_buffer_take(leaf);
        char *dump_path = ncc_temp_workspace_join(&tmp, leaf_text);
        ncc_free(leaf_text);

        bool absent = false;
        char *dump_err = nullptr;
        if (dump_section(objcopy, obj_path, ct_section_names[i], dump_path,
                         &absent, &dump_err)) {
            size_t len = 0;
            char *data = read_binary_file(dump_path, &len, err_out);
            if (!data) {
                ncc_free(dump_path);
                ncc_temp_workspace_cleanup(&tmp);
                return false;
            }

            int base_records = out->n_records;
            ok = parse_metadata_bytes((const unsigned char *)data, len, out,
                                      err_out);
            if (!ok) {
                truncate_record_list(out, base_records);
            }
            ncc_free(data);
            ncc_free(dump_path);
            if (ok) {
                out->n_objects_scanned++;
            }
            ncc_temp_workspace_cleanup(&tmp);
            return ok;
        }

        if (!absent) {
            saw_only_absent = false;
            set_err(err_out, "%s", dump_err ? dump_err
                                            : "objcopy section dump failed");
            ncc_free(dump_err);
            ncc_free(dump_path);
            ncc_temp_workspace_cleanup(&tmp);
            return false;
        }

        ncc_free(dump_err);
        ncc_free(dump_path);
    }

    if (saw_only_absent) {
        out->n_objects_scanned++;
        ncc_temp_workspace_cleanup(&tmp);
        return true;
    }

    ncc_temp_workspace_cleanup(&tmp);
    return false;
}

bool
ncc_ct_aggregate(const ncc_ct_rec_list_t *recs, ncc_ct_aggregate_t *out,
                 char **err_out)
{
    if (err_out) {
        *err_out = nullptr;
    }

    if (!recs || !out) {
        set_err(err_out, "ncc_ct_aggregate requires records and output");
        return false;
    }

    *out = (ncc_ct_aggregate_t){
        .n_objects_scanned = recs->n_objects_scanned,
    };

    for (int i = 0; i < recs->n_records; i++) {
        const ncc_ct_rec_t *rec = &recs->records[i];

        if (rec->kind == NCC_CT_REC_COMPTIME_MAIN) {
            if (!main_flags_valid(rec->main_flags)) {
                set_err(err_out, "invalid comptime_main flags 0x%02x",
                        (unsigned)rec->main_flags);
                ncc_ct_aggregate_free(out);
                return false;
            }
            if (!out->has_comptime_main) {
                out->has_comptime_main = true;
                out->main_sig = rec->sig;
                out->main_flags = rec->main_flags;
            }
            else if (!sig_eq(out->main_sig, rec->sig)) {
                set_err(err_out, "conflicting comptime_main signatures");
                ncc_ct_aggregate_free(out);
                return false;
            }
            else if (out->main_flags != rec->main_flags) {
                set_err(err_out, "conflicting comptime_main flags");
                ncc_ct_aggregate_free(out);
                return false;
            }
        }
        else if (rec->kind == NCC_CT_REC_VAR) {
            if (!var_fields_valid(&rec->var)) {
                set_err(err_out, "invalid comptime variable metadata '%.*s'",
                        (int)rec->var.name.u8_bytes, rec->var.name.data);
                ncc_ct_aggregate_free(out);
                return false;
            }

            bool duplicate = false;
            for (int j = 0; j < out->n_vars; j++) {
                if (!ncc_string_eq(out->vars[j].name, rec->var.name)) {
                    continue;
                }

                duplicate = true;
                if (out->vars[j].typehash != rec->var.typehash
                    || out->vars[j].linkage != rec->var.linkage
                    || out->vars[j].flags != rec->var.flags) {
                    set_err(err_out, "conflicting comptime variable '%.*s'",
                            (int)rec->var.name.u8_bytes, rec->var.name.data);
                    ncc_ct_aggregate_free(out);
                    return false;
                }
                break;
            }

            if (!duplicate) {
                append_aggregate_var(out, &rec->var);
            }
        }
        else if (rec->kind == NCC_CT_REC_STATIC_INIT) {
            if (!static_init_fields_valid(&rec->static_init)) {
                set_err(err_out, "invalid static-init metadata '%.*s'",
                        (int)rec->static_init.name.u8_bytes,
                        rec->static_init.name.data);
                ncc_ct_aggregate_free(out);
                return false;
            }

            bool duplicate = false;
            for (int j = 0; j < out->n_static_inits; j++) {
                if (!ncc_string_eq(out->static_inits[j].name,
                                   rec->static_init.name)) {
                    continue;
                }

                duplicate = true;
                if (out->static_inits[j].typehash
                        != rec->static_init.typehash
                    || out->static_inits[j].kind != rec->static_init.kind
                    || out->static_inits[j].flags != rec->static_init.flags
                    || out->static_inits[j].degrade_ok
                           != rec->static_init.degrade_ok) {
                    set_err(err_out, "conflicting static initializer '%.*s'",
                            (int)rec->static_init.name.u8_bytes,
                            rec->static_init.name.data);
                    ncc_ct_aggregate_free(out);
                    return false;
                }
                break;
            }

            if (!duplicate) {
                append_aggregate_static_init(out, &rec->static_init);
            }
        }
    }

    return true;
}

bool
ncc_ct_strip_section(const ncc_opts_t *opts, const char *binary_path,
                     char **err_out)
{
    (void)opts;

    if (err_out) {
        *err_out = nullptr;
    }

    if (!binary_path) {
        set_err(err_out, "ncc_ct_strip_section requires a path");
        return false;
    }

#ifdef _WIN32
    /*
     * llvm-objcopy removes the PE section without removing base-relocation
     * entries that target it. The Windows loader rejects that image with
     * ERROR_BAD_EXE_FORMAT, so retain the inert metadata section on PE.
     */
    return true;
#endif

    const char *objcopy = find_objcopy();
    if (!objcopy) {
        set_err(err_out, "llvm-objcopy or objcopy is required");
        return false;
    }

    ncc_temp_workspace_t tmp = {0};
    char *tmp_err = nullptr;
    if (!ncc_temp_workspace_create(&tmp, "ncc_ct_strip_", &tmp_err)) {
        set_err(err_out, "%s", tmp_err ? tmp_err
                                       : "failed to create temp workspace");
        ncc_free(tmp_err);
        return false;
    }

    bool  changed = false;
    char *current = nullptr;

    for (size_t i = 0; i < sizeof(ct_section_names) / sizeof(ct_section_names[0]);
         i++) {
        ncc_buffer_t *leaf = ncc_buffer_empty();
        ncc_buffer_printf(leaf, "stripped-%zu.o", i);
        char *leaf_text = ncc_buffer_take(leaf);
        char *out_path = ncc_temp_workspace_join(&tmp, leaf_text);
        ncc_free(leaf_text);

        bool absent = false;
        char *remove_err = nullptr;
        if (remove_section(objcopy, current ? current : binary_path,
                           ct_section_names[i], out_path, &absent,
                           &remove_err)) {
            ncc_free(current);
            current = out_path;
            changed = true;
            ncc_free(remove_err);
            continue;
        }

        if (!absent) {
            set_err(err_out, "%s", remove_err ? remove_err
                                              : "objcopy section remove failed");
            ncc_free(remove_err);
            ncc_free(out_path);
            ncc_free(current);
            ncc_temp_workspace_cleanup(&tmp);
            return false;
        }

        ncc_free(remove_err);
        ncc_free(out_path);
    }

    if (changed) {
        size_t len = 0;
        char *data = read_binary_file(current, &len, err_out);
        if (!data) {
            ncc_free(current);
            ncc_temp_workspace_cleanup(&tmp);
            return false;
        }

        bool wrote = ncc_platform_write_file(binary_path, data, len, err_out);
        ncc_free(data);
        ncc_free(current);
        ncc_temp_workspace_cleanup(&tmp);
        return wrote;
    }

    ncc_free(current);
    ncc_temp_workspace_cleanup(&tmp);
    return true;
}
