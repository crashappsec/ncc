/* WP-012 Phase 4: regression test that prevents drift between
 * ncc's standalone static-image helper stub (`test/static_image_helper.c`)
 * and n00b's production helper (`src/tools/n00b-static-init-helper.c`).
 *
 * Background
 * ----------
 * Both helpers consume the same text-protocol request and emit C source
 * for static-image objects.  WP-011 Phase 5e brought them into alignment
 * on buffer cached_hash emission; this test enforces that ALL future
 * shared-protocol drift in the represented cases is caught at test time.
 *
 * Approach: subprocess against both helpers when available, falling
 * back to a stub-only sanity check when the n00b helper binary cannot
 * be located.  The comparison is field-subset structural, not byte-
 * exact: every (block_name, field) pair the stub emits must also be
 * present in production with an equivalent value.  Production may emit
 * additional fields the stub doesn't (the stub is a simplified subset).
 *
 *   * The production helper's path defaults to
 *     `/Users/viega/n00b/.workspaces/static-generated-objects/build/
 *      n00b-static-init-helper` (the canonical workspace path).
 *   * Override via env var `N00B_STATIC_INIT_HELPER`.
 *   * Override via argv[2] (meson passes the stub as argv[1]).
 *   * If the production helper is absent, the test logs a "skip
 *     production" notice and validates the stub's NCC_STATIC_INIT_OK
 *     marker + non-empty body so trivial regressions still get caught.
 *
 * Tradeoff
 * --------
 * Live subprocess against both helpers catches production drift but
 * requires the n00b helper to be built.  CI environments that build
 * only ncc will silently skip the cross-helper check (production drift
 * is caught separately by n00b's own tests).  The alternative (commit
 * a golden snapshot of stub output) would catch stub drift but not
 * production drift; we picked subprocess for tighter cross-coverage.
 *
 * Normalization
 * -------------
 * The comparator strips per-run identity from both outputs so the
 * structural comparison succeeds even though hashes / file paths /
 * symbol prefixes differ across cases:
 *   - whitespace runs → single space.
 *   - integer-literal suffixes (`ULL`, `u`) → empty.
 *   - 16-hex-digit hex constants (XXH3 hashes) → `HASH64`.
 *   - file paths in `.file=` slots → `FILE`.
 *   - C++/C99 comments → empty.
 *   - The shared symbol-prefix that begins each request (e.g.
 *     `__test_xxx_`) → `PFX_`.
 *   - Known equivalences are collapsed:
 *       * `nullptr` ↔ `0`  → `NULL`.
 *       * `__attribute__((used))` → empty (production emits it after
 *         `_lock_entry` / `_obj_entry`; stub omits it).
 *       * lock cast `(n00b_rwlock_t*)PFX_lock_storage` and direct
 *         `&PFX_lock` → both → `LOCK_REF` (D-070 lock-model
 *         divergence: production uses storage-byte-array, stub uses
 *         direct typed instance).
 *       * `1u` / `1` (request `version`, `endian`) → `1`.
 *
 * Comparison
 * ----------
 * After normalization, both outputs are parsed into named blocks:
 *
 *   static <quals> <type> <NAME>[<...>] = { <body> };
 *
 * For each `<NAME>` the stub emits, the test requires the same
 * `<NAME>` to be present in production AND every `.<field>=<value>`
 * pair in the stub's body to be present in production's body for that
 * block.  Production-only blocks and production-only fields are
 * tolerated (the stub may emit a subset).
 *
 * Cases (matches the WP-011 protocol surface):
 *   1. r-string emission (rstr template-plain path).            [SKIPPED]
 *   2. buffer emission.                                         [SKIPPED]
 *   3. small list of scalar elements.
 *   4. small array of scalar elements.
 *   5. dict of scalar keys.
 *   6. dict of r-string keys.
 *   7. dict of buffer keys.
 *
 * The r-string and buffer cases are tracked but currently SKIPPED in
 * the cross-helper comparison because the stub's standalone emission
 * for those payload types diverges from the production helper's
 * registered-type code path (production looks up a type-registered
 * static initializer; the stub hardcodes the buffer/test type slots).
 * They are still exercised against the stub for a basic sanity check
 * (non-empty `NCC_STATIC_INIT_OK` response), preserving partial drift
 * detection on the stub side.  Container cases (list, array, dict)
 * route both helpers through their direct emit_* functions and DO
 * exercise the field-subset comparator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "util/platform.h"

#define MAX_NORMALIZED_LEN (256u * 1024u)

typedef struct {
    const char *name;          /* short label used in test output. */
    const char *request;       /* request text fed to the helper.  */
    const char *symbol_prefix; /* matches `prefix <X>` in request. */
    bool        cross_helper;  /* if true, compare stub vs prod.   */
} drift_case_t;

/* ---------------------------------------------------------------- */
/* Normalization.                                                    */
/* ---------------------------------------------------------------- */

/* Replace every occurrence of `needle` with `replacement` in `buf`
 * (in-place).  Both strings are NUL-terminated.  This is a simple
 * O(n*m) implementation suitable for the small bodies under test.
 */
static void
str_replace_all(char *buf, const char *needle, const char *replacement)
{
    size_t needle_len = strlen(needle);
    size_t repl_len   = strlen(replacement);
    if (needle_len == 0) {
        return;
    }

    char *p = buf;
    while ((p = strstr(p, needle)) != nullptr) {
        size_t tail_len = strlen(p + needle_len);
        if (repl_len <= needle_len) {
            memmove(p + repl_len, p + needle_len, tail_len + 1);
            memcpy(p, replacement, repl_len);
            p += repl_len;
        }
        else {
            /* Growth path: tail must shift right, which can blow the
             * buffer if we don't budget for it.  All inputs here are
             * shrink-only, so we error out rather than risk overflow.
             */
            fprintf(stderr,
                    "BUG: str_replace_all growth not supported\n");
            abort();
        }
    }
}

/* Strip C/C++ comments.  Single-line `// ...\n` is collapsed to
 * a newline; block /-star ... star-/ is collapsed to a space.  Neither
 * helper emits real comments today, but normalization defends against
 * that drift category if it ever appears.
 */
static void
strip_comments(char *buf)
{
    char *src = buf;
    char *dst = buf;
    while (*src) {
        if (src[0] == '/' && src[1] == '/') {
            while (*src && *src != '\n') {
                src++;
            }
            continue;
        }
        if (src[0] == '/' && src[1] == '*') {
            src += 2;
            while (*src) {
                if (src[0] == '*' && src[1] == '/') {
                    src += 2;
                    break;
                }
                src++;
            }
            *dst++ = ' ';
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Replace 64-bit hex constants (`0x[0-9a-fA-F]{16}` optionally with
 * `ULL`) with the literal token `HASH64`.  XXH3 hashes, object_id
 * values, and bucket `hv` halves all match this pattern.
 */
static void
normalize_hash_hex(char *buf)
{
    char *src = buf;
    char *dst = buf;
    while (*src) {
        if (src[0] == '0' && (src[1] == 'x' || src[1] == 'X')) {
            char *p = src + 2;
            int   count = 0;
            while (isxdigit((unsigned char)*p) && count < 17) {
                p++;
                count++;
            }
            if (count == 16) {
                while (*p == 'U' || *p == 'u' || *p == 'L' || *p == 'l') {
                    p++;
                }
                memcpy(dst, "HASH64", 6);
                dst += 6;
                src = p;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Replace decimal integer-literal suffixes (`ULL`, `LL`, `UL`, `L`,
 * `u`) when they immediately follow a digit run.  This collapses the
 * production helper's `(size_t)2ULL` ↔ stub's `(size_t)2` divergence.
 */
static void
strip_integer_suffixes(char *buf)
{
    char *src = buf;
    char *dst = buf;
    while (*src) {
        *dst++ = *src;
        if (isdigit((unsigned char)*src)) {
            char *p = src + 1;
            while (isdigit((unsigned char)*p)) {
                *dst++ = *p++;
            }
            /* p now points past the digit run. */
            while (*p == 'U' || *p == 'u' || *p == 'L' || *p == 'l') {
                p++;
            }
            src = p;
            continue;
        }
        src++;
    }
    *dst = '\0';
}

/* Collapse runs of whitespace to a single space.  Newlines become
 * spaces too; we compare bodies as a flat token stream.
 */
static void
collapse_whitespace(char *buf)
{
    char *src = buf;
    char *dst = buf;
    int   in_ws = 0;
    while (*src) {
        if (isspace((unsigned char)*src)) {
            if (!in_ws) {
                *dst++ = ' ';
                in_ws  = 1;
            }
            src++;
        }
        else {
            in_ws  = 0;
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Replace the file token in `.file=__FILE__` (always the same source
 * file in the helpers, but normalize anyway for robustness in case a
 * helper ever quotes a real path).
 */
static void
normalize_file_tokens(char *buf)
{
    str_replace_all(buf, "__FILE__", "FILE");
    /* If a helper ever emits a quoted path, replace any `"...c"` token
     * preceded by `.file=` — but our current helpers always use
     * `__FILE__` so this is conservative future-proofing only.
     */
}

/* Replace `__attribute__((used))` with an empty marker (production
 * emits this on lock_entry / data_entry; the stub omits it because
 * the entry_attr is configured per-test via ncc flags rather than
 * baked into the helper output).
 */
static void
strip_used_attr(char *buf)
{
    str_replace_all(buf, "__attribute__((used))", "");
    str_replace_all(buf, "__attribute__ ((used))", "");
}

/* Collapse the D-070 lock-model divergence: production emits
 * `(n00b_rwlock_t*)PFX_lock_storage`, stub emits `&PFX_lock` after
 * the symbol-prefix has been canonicalized to `PFX_`.  Run AFTER the
 * symbol-prefix normalization.
 */
static void
normalize_lock_ref(char *buf)
{
    /* Production form after symbol-prefix normalization. */
    str_replace_all(buf, "(n00b_rwlock_t*)PFX_lock_storage", "LOCK_REF");
    /* Stub form. */
    str_replace_all(buf, "&PFX_lock", "LOCK_REF");
}

/* Replace the test's symbol prefix everywhere it appears as a token
 * with the literal `PFX_`.  Both helpers prepend the request's `prefix
 * <X>` value to every identifier they emit (e.g. `<prefix>_data`,
 * `<prefix>_buckets`).  We use plain string replace because the prefix
 * is chosen to be unique per case and won't appear as a substring of
 * any helper-emitted keyword.
 */
static void
normalize_symbol_prefix(char *buf, const char *prefix)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "%s", prefix);
    str_replace_all(buf, needle, "PFX");
}

/* Collapse `nullptr` vs `0` divergence.  Both are valid for pointer
 * slots; production uses `nullptr`, stub uses `0` for several legacy
 * paths.  We compare equivalence by mapping both to `NULL`.
 */
/* In-place rewrite of `<slot>=0` (where the `0` is a standalone
 * pointer-null constant) to `<slot>=NULL`.  We must NOT touch
 * `<slot>=0x...` (hex), `<slot>=0,...` where 0 is a real integer in a
 * non-pointer slot, or `<slot>=04` (octal).  This helper scans for the
 * literal slot tag, verifies the value is exactly `0` followed by `,`,
 * `}`, ` `, or end-of-string, and rewrites to `NULL`.  Because `NULL`
 * is 4 chars and `0` is 1 char, we need an in-place grow that shifts
 * the tail.  We do this safely by walking once with a memmove per
 * match.
 */
static void
rewrite_null_pointer_slot(char *buf, const char *slot_tag)
{
    size_t tag_len = strlen(slot_tag);
    char  *p = buf;
    while ((p = strstr(p, slot_tag)) != nullptr) {
        char *value = p + tag_len;
        if (*value != '0') {
            p++;
            continue;
        }
        char term = value[1];
        if (term != ',' && term != '}' && term != ' '
            && term != '\0' && term != ';') {
            /* `0x...` or `04`: not a bare null. */
            p++;
            continue;
        }
        /* Shift tail right by 3 to make room for `NULL`. */
        size_t tail_len = strlen(value + 1);
        memmove(value + 4, value + 1, tail_len + 1);
        value[0] = 'N';
        value[1] = 'U';
        value[2] = 'L';
        value[3] = 'L';
        p = value + 4;
    }
}

static void
normalize_null(char *buf)
{
    str_replace_all(buf, "nullptr", "NULL");
    /* The stub still uses `0` for several pointer slots (`.identity=0`,
     * `.scan_cb=0`, etc.) where production uses `nullptr`.  Map known
     * pointer-slot `=0` patterns to `=NULL` so they don't surface as
     * spurious drift.  The list is conservative: it only touches slots
     * that BOTH helpers populate with a null-pointer constant when no
     * identity / callback is configured.
     */
    static const char *const null_slots[] = {
        ".identity=", ".scan_cb=", ".scan_user=",
        ".file=", ".lock=", ".allocator=",
        ".identity_namespace=", ".identity_object_key=",
        ".identity_payload_key=", ".args=", ".payload=",
        ".object_start=",
        nullptr,
    };
    for (size_t i = 0; null_slots[i]; i++) {
        rewrite_null_pointer_slot(buf, null_slots[i]);
    }
}

/* Collapse symbolic `N00B_GC_SCAN_KIND_*` names ↔ their integer
 * equivalents (defined in include/n00b/core/gc_scan_kind.h):
 *   DEFAULT  = 0
 *   NONE     = 1
 *   ALL      = 2
 *   CALLBACK = 4
 * Both helpers freely interchange the integer literal and the macro,
 * and the C compiler treats them as identical anyway.
 */
static void
normalize_scan_kinds(char *buf)
{
    str_replace_all(buf, "N00B_GC_SCAN_KIND_DEFAULT", "0");
    str_replace_all(buf, "N00B_GC_SCAN_KIND_NONE", "1");
    str_replace_all(buf, "N00B_GC_SCAN_KIND_ALL", "2");
    str_replace_all(buf, "N00B_GC_SCAN_KIND_CALLBACK", "4");
}

/* Collapse symbolic object-flag names ↔ integers.  Flag bit values:
 *   N00B_STATIC_OBJECT_F_READONLY    = 1
 *   N00B_STATIC_OBJECT_F_MUTABLE     = 2
 *   N00B_STATIC_OBJECT_F_INIT_RWLOCK = 4
 * Combined values appear as `A|B`; we don't normalize multi-bit
 * combinations (the helpers don't combine these in fields the stub
 * emits).  Stub uses the integer literal, production uses the macro
 * — map both to the integer for a consistent compare.
 */
static void
normalize_object_flags(char *buf)
{
    str_replace_all(buf,
                    "N00B_STATIC_OBJECT_F_MUTABLE|"
                    "N00B_STATIC_OBJECT_F_INIT_RWLOCK", "6");
    str_replace_all(buf, "N00B_STATIC_OBJECT_F_READONLY", "1");
    str_replace_all(buf, "N00B_STATIC_OBJECT_F_MUTABLE", "2");
    str_replace_all(buf, "N00B_STATIC_OBJECT_F_INIT_RWLOCK", "4");
}

/* Full normalization pipeline. */
static void
normalize(char *buf, const char *symbol_prefix)
{
    strip_comments(buf);
    normalize_hash_hex(buf);
    strip_integer_suffixes(buf);
    normalize_file_tokens(buf);
    normalize_symbol_prefix(buf, symbol_prefix);
    normalize_lock_ref(buf);
    strip_used_attr(buf);
    normalize_scan_kinds(buf);
    normalize_object_flags(buf);
    collapse_whitespace(buf);
    /* normalize_null must run AFTER whitespace collapse because the
     * `=0` patterns match exactly without intervening spaces.
     */
    normalize_null(buf);
}

/* ---------------------------------------------------------------- */
/* Field-subset comparison.                                          */
/* ---------------------------------------------------------------- */

/* Find the index of the `=` that opens the initializer for the named
 * static block, scanning the normalized buffer for tokens of the form
 *   `static ... <NAME>{` or `static ... <NAME>=` or `static ... <NAME>[N]={`.
 * Returns a pointer to the `{` of the initializer, or NULL if not
 * found.  `block_name_out`, if non-NULL, receives a copy of the bare
 * identifier (without trailing `[...]`).
 */
static const char *
find_block_open(const char *buf, const char *name, size_t name_len)
{
    const char *p = buf;
    while ((p = strstr(p, name)) != nullptr) {
        /* Identifier boundary check. */
        if (p > buf && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
            p++;
            continue;
        }
        const char *q = p + name_len;
        /* Skip optional `[...]` array dim and surrounding whitespace. */
        while (*q == ' ') {
            q++;
        }
        if (*q == '[') {
            while (*q && *q != ']') {
                q++;
            }
            if (*q == ']') {
                q++;
            }
            while (*q == ' ') {
                q++;
            }
        }
        if (*q == '=') {
            q++;
            while (*q == ' ') {
                q++;
            }
        }
        if (*q == '{') {
            return q;
        }
        p++;
    }
    return nullptr;
}

/* Copy the body between the matching `{` and `}` into `out` (max
 * `cap` bytes including NUL).  Tracks nested braces.  Returns true on
 * success, false if the body would overflow `cap` or the braces are
 * unbalanced.
 */
static bool
copy_block_body(const char *open_brace, char *out, size_t cap)
{
    if (*open_brace != '{') {
        return false;
    }
    int    depth = 0;
    size_t i     = 0;
    const char *p = open_brace;
    while (*p) {
        if (*p == '{') {
            depth++;
            if (depth == 1) {
                p++;
                continue;
            }
        }
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                if (i >= cap) {
                    return false;
                }
                out[i] = '\0';
                return true;
            }
        }
        if (i + 1 >= cap) {
            return false;
        }
        out[i++] = *p++;
    }
    return false;
}

/* Walk a normalized block body (the contents between the outermost
 * `{` and `}`, with whitespace already collapsed).  For each top-
 * level `.field=value` pair (skipping nested braces / parens), invoke
 * `cb(field, value, user)`.  `field` is NUL-terminated; `value` is
 * NOT NUL-terminated — pass `value_len` to the callback.  Returns
 * true if iteration completed without parse error.
 */
typedef void (*pair_cb_t)(const char *field, const char *value,
                          size_t value_len, void *user);

static bool
walk_pairs(const char *body, pair_cb_t cb, void *user)
{
    const char *p = body;
    int         depth = 0;   /* nested brace depth.   */
    int         paren = 0;   /* nested paren depth.   */
    int         bracket = 0; /* nested bracket depth. */
    while (*p == ' ') {
        p++;
    }
    while (*p) {
        /* Track nesting so we only treat top-level `.field=value`
         * tokens as designated initializers.  Bucket initializers,
         * abi sub-structs, etc. are skipped — their fields belong to
         * the nested block, not the outer one.
         */
        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            if (depth > 0) {
                depth--;
            }
            p++;
            continue;
        }
        if (*p == '(') {
            paren++;
            p++;
            continue;
        }
        if (*p == ')') {
            if (paren > 0) {
                paren--;
            }
            p++;
            continue;
        }
        if (*p == '[') {
            bracket++;
            p++;
            continue;
        }
        if (*p == ']') {
            if (bracket > 0) {
                bracket--;
            }
            p++;
            continue;
        }
        if (*p != '.' || depth != 0 || paren != 0 || bracket != 0) {
            p++;
            continue;
        }
        p++; /* past the leading '.' */
        const char *field_start = p;
        while (*p && *p != '=' && *p != ',' && *p != '{' && *p != ' ') {
            p++;
        }
        if (*p != '=') {
            /* Designated init must have `=`; otherwise skip. */
            continue;
        }
        size_t field_len = (size_t)(p - field_start);
        if (field_len == 0 || field_len > 127) {
            continue;
        }
        char field[128];
        memcpy(field, field_start, field_len);
        field[field_len] = '\0';

        p++; /* past '=' */
        const char *value_start = p;
        int paren = 0;
        int brace = 0;
        int bracket = 0;
        while (*p) {
            if (*p == '(') {
                paren++;
            }
            else if (*p == ')') {
                paren--;
            }
            else if (*p == '{') {
                brace++;
            }
            else if (*p == '}') {
                if (brace == 0) {
                    break;
                }
                brace--;
            }
            else if (*p == '[') {
                bracket++;
            }
            else if (*p == ']') {
                bracket--;
            }
            else if (*p == ',' && paren == 0 && brace == 0 && bracket == 0) {
                break;
            }
            p++;
        }
        size_t value_len = (size_t)(p - value_start);
        /* Trim trailing whitespace. */
        while (value_len > 0
               && isspace((unsigned char)value_start[value_len - 1])) {
            value_len--;
        }
        if (cb) {
            cb(field, value_start, value_len, user);
        }
        if (*p == ',') {
            p++;
        }
    }
    return true;
}

/* Compare blocks: for every (field, value) pair in `stub_body`,
 * the same field must exist in `prod_body` with the same value.
 * Records the first mismatch in `*err_field` / `*err_stub` /
 * `*err_prod` (caller-provided buffers).
 */
typedef struct {
    const char *prod_body;
    const char *block_name;
    bool        had_error;
    char        err_field[128];
    char        err_stub[512];
    char        err_prod[512];
} compare_ctx_t;

/* Locate the value (NUL-terminated, in `out`) for a given field in
 * `body`.  Returns true if found.  Comparisons are exact-string after
 * normalization.
 */
static bool
find_field_value(const char *body, const char *field, char *out,
                 size_t cap)
{
    char target[160];
    int  n = snprintf(target, sizeof(target), ".%s=", field);
    if (n <= 0 || (size_t)n >= sizeof(target)) {
        return false;
    }
    size_t target_len = (size_t)n;

    /* Scan the body tracking nesting depth so the lookup only matches
     * top-level `.field=` tokens; nested struct sub-initializers
     * (bucket entries, abi sub-struct, etc.) are skipped.
     */
    const char *p = body;
    int         depth = 0;
    int         paren = 0;
    int         bracket = 0;
    while (*p) {
        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            if (depth > 0) {
                depth--;
            }
            p++;
            continue;
        }
        if (*p == '(') {
            paren++;
            p++;
            continue;
        }
        if (*p == ')') {
            if (paren > 0) {
                paren--;
            }
            p++;
            continue;
        }
        if (*p == '[') {
            bracket++;
            p++;
            continue;
        }
        if (*p == ']') {
            if (bracket > 0) {
                bracket--;
            }
            p++;
            continue;
        }
        if (depth != 0 || paren != 0 || bracket != 0
            || strncmp(p, target, target_len) != 0) {
            p++;
            continue;
        }
        if (p > body && (isalnum((unsigned char)p[-1]) || p[-1] == '_'
                         || p[-1] == '.')) {
            p++;
            continue;
        }
        p += target_len;
        const char *value_start = p;
        int paren = 0;
        int brace = 0;
        int bracket = 0;
        while (*p) {
            if (*p == '(') {
                paren++;
            }
            else if (*p == ')') {
                paren--;
            }
            else if (*p == '{') {
                brace++;
            }
            else if (*p == '}') {
                if (brace == 0) {
                    break;
                }
                brace--;
            }
            else if (*p == '[') {
                bracket++;
            }
            else if (*p == ']') {
                bracket--;
            }
            else if (*p == ',' && paren == 0 && brace == 0 && bracket == 0) {
                break;
            }
            p++;
        }
        size_t value_len = (size_t)(p - value_start);
        while (value_len > 0
               && isspace((unsigned char)value_start[value_len - 1])) {
            value_len--;
        }
        if (value_len + 1 > cap) {
            return false;
        }
        memcpy(out, value_start, value_len);
        out[value_len] = '\0';
        return true;
    }
    return false;
}

static void
visit_stub_pair(const char *field, const char *value, size_t value_len,
                void *user)
{
    compare_ctx_t *ctx = (compare_ctx_t *)user;
    if (ctx->had_error) {
        return;
    }

    /* Reject pairs nested inside a parent value: `walk_pairs` only
     * yields top-level pairs by tracking brace/paren/bracket depth,
     * so we trust it here.  The callback receives field NUL-terminated
     * and value as (ptr, len).
     */

    /* Some fields are STRUCTURAL aliases we treat as equivalent.  See
     * the file header for the rationale of each.  When the stub and
     * production names differ but mean the same thing, the comparator
     * skips the pair (it's covered by another, equivalent assertion).
     */

    char stub_value[512];
    if (value_len + 1 > sizeof(stub_value)) {
        ctx->had_error = true;
        snprintf(ctx->err_field, sizeof(ctx->err_field), "%s", field);
        snprintf(ctx->err_stub, sizeof(ctx->err_stub),
                 "<oversized value, len=%zu>", value_len);
        snprintf(ctx->err_prod, sizeof(ctx->err_prod), "<n/a>");
        return;
    }
    memcpy(stub_value, value, value_len);
    stub_value[value_len] = '\0';

    char prod_value[512];
    if (!find_field_value(ctx->prod_body, field, prod_value,
                          sizeof(prod_value))) {
        /* Production may legitimately omit fields the stub emits if
         * the stub leans on a default.  We treat absence as drift
         * because the stub explicitly chose to populate the slot;
         * silence here would let a slot disappear from production
         * without notice.
         */
        ctx->had_error = true;
        snprintf(ctx->err_field, sizeof(ctx->err_field), "%s", field);
        snprintf(ctx->err_stub, sizeof(ctx->err_stub), "%s", stub_value);
        snprintf(ctx->err_prod, sizeof(ctx->err_prod), "<missing>");
        return;
    }

    if (strcmp(stub_value, prod_value) != 0) {
        ctx->had_error = true;
        snprintf(ctx->err_field, sizeof(ctx->err_field), "%s", field);
        snprintf(ctx->err_stub, sizeof(ctx->err_stub), "%s", stub_value);
        snprintf(ctx->err_prod, sizeof(ctx->err_prod), "%s", prod_value);
        return;
    }
}

static bool
compare_block(const char *stub_buf, const char *prod_buf,
              const char *block_name, char *err_msg, size_t err_cap)
{
    size_t name_len = strlen(block_name);
    const char *stub_open = find_block_open(stub_buf, block_name, name_len);
    if (!stub_open) {
        /* Stub didn't emit this block — that's fine; the test maps the
         * stub's emission as the subset to enforce.
         */
        return true;
    }
    const char *prod_open = find_block_open(prod_buf, block_name, name_len);
    if (!prod_open) {
        snprintf(err_msg, err_cap,
                 "block '%s' emitted by stub but missing from production",
                 block_name);
        return false;
    }

    char stub_body[16 * 1024];
    char prod_body[32 * 1024];
    if (!copy_block_body(stub_open, stub_body, sizeof(stub_body))) {
        snprintf(err_msg, err_cap,
                 "could not copy stub body for block '%s'", block_name);
        return false;
    }
    if (!copy_block_body(prod_open, prod_body, sizeof(prod_body))) {
        snprintf(err_msg, err_cap,
                 "could not copy production body for block '%s'",
                 block_name);
        return false;
    }

    compare_ctx_t ctx = {
        .prod_body  = prod_body,
        .block_name = block_name,
        .had_error  = false,
    };
    walk_pairs(stub_body, visit_stub_pair, &ctx);
    if (ctx.had_error) {
        snprintf(err_msg, err_cap,
                 "block '%s' field '.%s' diverges: stub=%s prod=%s",
                 block_name, ctx.err_field, ctx.err_stub, ctx.err_prod);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Subprocess driver.                                                */
/* ---------------------------------------------------------------- */

static bool
run_helper(const char *path, const char *request, char **out_stdout,
           size_t *out_stdout_len, char **out_err)
{
    const char *argv[] = {path, nullptr};
    ncc_process_spec_t spec = {
        .program        = path,
        .argv           = argv,
        .stdin_data     = request,
        .stdin_len      = strlen(request),
        .capture_stdout = true,
        .capture_stderr = true,
    };
    ncc_process_result_t result;
    if (!ncc_process_run(&spec, &result)) {
        *out_err = result.stderr_data
                       ? strdup(result.stderr_data)
                       : strdup("ncc_process_run failed");
        ncc_process_result_free(&result);
        return false;
    }
    if (result.exit_code != 0) {
        size_t cap = (result.stderr_len ? result.stderr_len : 0u) + 128u;
        *out_err = malloc(cap);
        snprintf(*out_err, cap,
                 "helper '%s' exited %d: %.*s",
                 path, result.exit_code, (int)result.stderr_len,
                 result.stderr_data ? result.stderr_data : "");
        ncc_process_result_free(&result);
        return false;
    }
    *out_stdout     = result.stdout_data;
    *out_stdout_len = result.stdout_len;
    result.stdout_data = nullptr;
    result.stdout_len  = 0;
    ncc_process_result_free(&result);
    return true;
}

/* ---------------------------------------------------------------- */
/* Canonical request fixtures.                                       */
/* ---------------------------------------------------------------- */

/* List of 2 ints (inline, mutable).  Exercises emit_list_image. */
static const char list_request[] =
    "NCC_STATIC_INIT 1\n"
    "type_hex 6e3030625f6c6973745f7428696e7429\n"
    "type_hash 999\n"
    "prefix __ncc_drift_list\n"
    "container_kind list\n"
    "container_target value\n"
    "readonly 0\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "element_type_hex 696e74\n"
    "element_type_hash 123\n"
    "data_type_hash 456\n"
    "len 2\n"
    "cap 2\n"
    "arg_count 2\n"
    "arg - cinit 1 31\n"
    "arg - cinit 1 32\n"
    "end\n";

/* Array of 2 ints (matches list shape but routes to emit_array_image). */
static const char array_request[] =
    "NCC_STATIC_INIT 1\n"
    "type_hex 696e74\n"
    "type_hash 888\n"
    "prefix __ncc_drift_array\n"
    "container_kind array\n"
    "container_target value\n"
    "readonly 0\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "element_type_hex 696e74\n"
    "element_type_hash 123\n"
    "data_type_hash 456\n"
    "len 2\n"
    "cap 2\n"
    "arg_count 2\n"
    "arg - cinit 1 31\n"
    "arg - cinit 1 32\n"
    "end\n";

/* Dict { 1: 10, 2: 20 } with scalar (int) keys.  Hash values are made
 * up but mutually unique within mask=15 so probing converges.
 */
static const char scalar_dict_request[] =
    "NCC_STATIC_INIT 1\n"
    "container_kind dict\n"
    "container_target value\n"
    "type_hex 6e3030625f646963745f7428696e742c696e7429\n"
    "type_hash 777\n"
    "key_type_hex 696e74\n"
    "key_type_hash 123\n"
    "value_type_hex 696e74\n"
    "value_type_hash 123\n"
    "data_type_hash 555\n"
    "prefix __ncc_drift_sdict\n"
    "readonly 0\n"
    "len 2\n"
    "cap 16\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "key_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "key_scan_cb_hex 6e756c6c707472\n"
    "key_scan_user_hex 6e756c6c707472\n"
    "key_shape_decl_hex\n"
    "value_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "value_scan_cb_hex 6e756c6c707472\n"
    "value_scan_user_hex 6e756c6c707472\n"
    "value_shape_decl_hex\n"
    "skip_obj_hash 1\n"
    "cached_hash_emit no\n"
    "arg_count 2\n"
    "arg - pair cinit 1 31 2 3130 hash 1 0\n"
    "arg - pair cinit 1 32 2 3230 hash 2 0\n"
    "end\n";

/* Dict { rstr_key0: 100, rstr_key1: 200 } with pointer (r-string) keys.
 * Skip_obj_hash=0 (production calls n00b_hash on key pointer); cached
 * hash values are precomputed and threaded as `hash <lo> <hi>` modifiers.
 */
static const char rstring_key_dict_request[] =
    "NCC_STATIC_INIT 1\n"
    "container_kind dict\n"
    "container_target value\n"
    "type_hex 6e3030625f646963745f74286e30306225737472696e675f742a2c696e7429\n"
    "type_hash 666\n"
    "key_type_hex 6e30306225737472696e675f742a\n"
    "key_type_hash 321\n"
    "value_type_hex 696e74\n"
    "value_type_hash 123\n"
    "data_type_hash 654\n"
    "prefix __ncc_drift_rdict\n"
    "readonly 0\n"
    "len 2\n"
    "cap 16\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "key_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "key_scan_cb_hex 6e756c6c707472\n"
    "key_scan_user_hex 6e756c6c707472\n"
    "key_shape_decl_hex\n"
    "value_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "value_scan_cb_hex 6e756c6c707472\n"
    "value_scan_user_hex 6e756c6c707472\n"
    "value_shape_decl_hex\n"
    "skip_obj_hash 0\n"
    "cached_hash_emit yes\n"
    "arg_count 2\n"
    "arg - pair cinit 3 266b30 3 313030 hash 11 22\n"
    "arg - pair cinit 3 266b31 3 323030 hash 33 44\n"
    "end\n";

/* Dict { bk_0: 10, bk_1: 20 } with buffer keys.  Mirrors the r-string
 * case (skip_obj_hash=0, precomputed hashes) — production emits a
 * .cached_hash slot in the bucket entries; the test verifies the
 * stub puts the same hash bytes in the bucket `hv` slot.
 */
static const char buffer_key_dict_request[] =
    "NCC_STATIC_INIT 1\n"
    "container_kind dict\n"
    "container_target value\n"
    "type_hex 6e3030625f646963745f74286e30306225627566666572736d745f742a2c696e7429\n"
    "type_hash 555\n"
    "key_type_hex 6e30306225627566666572736d745f742a\n"
    "key_type_hash 333\n"
    "value_type_hex 696e74\n"
    "value_type_hash 123\n"
    "data_type_hash 543\n"
    "prefix __ncc_drift_bdict\n"
    "readonly 0\n"
    "len 2\n"
    "cap 16\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "key_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "key_scan_cb_hex 6e756c6c707472\n"
    "key_scan_user_hex 6e756c6c707472\n"
    "key_shape_decl_hex\n"
    "value_scan_kind N00B_GC_SCAN_KIND_NONE\n"
    "value_scan_cb_hex 6e756c6c707472\n"
    "value_scan_user_hex 6e756c6c707472\n"
    "value_shape_decl_hex\n"
    "skip_obj_hash 0\n"
    "cached_hash_emit yes\n"
    "arg_count 2\n"
    "arg - pair cinit 3 266230 3 313030 hash 55 66\n"
    "arg - pair cinit 3 266231 3 323030 hash 77 88\n"
    "end\n";

/* Buffer (r-byte-string) emission.  Production routes this through
 * `n00b_buffer_static_init` which requires the type registry to know
 * the buffer type — the stub answers directly without that lookup.
 * Cross-helper comparison is SKIPPED for this case; stub-only sanity
 * check ensures the response still starts with NCC_STATIC_INIT_OK.
 */
static const char buffer_request[] =
    "NCC_STATIC_INIT 1\n"
    "type_hex 6e3030625f6275666665725f74\n"
    "type_hash 17361917583128711234\n"
    "prefix __ncc_drift_buf\n"
    "readonly 1\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "identity_namespace_hex 6e7300\n"
    "identity_object_key_hex 6f626a6b6579\n"
    "identity_payload_key_hex 70617900\n"
    "arg_count 1\n"
    "arg - bytes 3 616263\n"
    "end\n";

/* r-string emission via `n00b_static_image_test_t`.  Production
 * routes the r-string template through its own runtime expansion;
 * the stub responds with a hand-rolled n00b_static_image_test_t.
 * Cross-helper comparison is SKIPPED; stub-only sanity check.
 */
static const char rstring_request[] =
    "NCC_STATIC_INIT 1\n"
    "type_hex 6e3030625f7374617469635f696d6167655f746573745f74\n"
    "type_hash 0\n"
    "prefix __ncc_drift_rstr\n"
    "readonly 1\n"
    "abi 8 8 8 1\n"
    "entry_attr_hex 5f5f6174747269627574655f5f2828757365642929\n"
    "identity_namespace_hex 6e7300\n"
    "identity_object_key_hex 6f626a6b6579\n"
    "identity_payload_key_hex 70617900\n"
    "arg_count 1\n"
    "arg - bytes 5 68656c6c6f\n"
    "end\n";

/* The set of named blocks the comparator enforces per container case.
 * These mirror the static declarations the helper emits.  Block names
 * are AFTER symbol-prefix normalization (so `PFX_<suffix>`).
 *
 * For list (value-target):
 *   PFX_data, PFX_data_desc, PFX_lock_desc, PFX_request, PFX_response
 * For array:
 *   PFX_data, PFX_data_desc
 * For dict (value-target):
 *   PFX_buckets, PFX_keys, PFX_values, PFX_store
 *
 * We restrict the comparator to per-case blocks that BOTH helpers
 * emit.  Per-case blocks that ONLY one side emits (e.g. the stub's
 * PFX_lock object descriptor, the production helper's
 * PFX_lock_storage byte array) are skipped by the comparator (they
 * would naturally fail the block-name match; the stub's emission is
 * the subset so those production-only blocks are tolerated).
 */
static const char *const list_blocks[] = {
    "PFX_data",
    "PFX_data_desc",
    nullptr,
};

static const char *const array_blocks[] = {
    "PFX_data",
    "PFX_data_desc",
    nullptr,
};

/* Dict-side blocks: both helpers emit buckets/keys/values/store.
 * The `_obj_desc` block is only produced by pointer-target dicts (the
 * value-target case both helpers use here doesn't emit it for the
 * stub).
 */
static const char *const dict_blocks[] = {
    "PFX_buckets",
    "PFX_keys",
    "PFX_values",
    "PFX_store",
    nullptr,
};

static const drift_case_t drift_cases[] = {
    /* Cross-helper container cases. */
    {.name = "list_2_ints", .request = list_request,
     .symbol_prefix = "__ncc_drift_list", .cross_helper = true},
    {.name = "array_2_ints", .request = array_request,
     .symbol_prefix = "__ncc_drift_array", .cross_helper = true},
    {.name = "dict_scalar_keys", .request = scalar_dict_request,
     .symbol_prefix = "__ncc_drift_sdict", .cross_helper = true},
    {.name = "dict_rstring_keys", .request = rstring_key_dict_request,
     .symbol_prefix = "__ncc_drift_rdict", .cross_helper = true},
    {.name = "dict_buffer_keys", .request = buffer_key_dict_request,
     .symbol_prefix = "__ncc_drift_bdict", .cross_helper = true},
    /* Stub-only sanity cases — production routes these through the
     * type-registry path which requires libn00b runtime state we can't
     * easily fabricate from a request alone.  We still drive the stub
     * to ensure its output keeps a non-empty NCC_STATIC_INIT_OK line.
     */
    {.name = "buffer", .request = buffer_request,
     .symbol_prefix = "__ncc_drift_buf", .cross_helper = false},
    {.name = "rstring", .request = rstring_request,
     .symbol_prefix = "__ncc_drift_rstr", .cross_helper = false},
};

/* ---------------------------------------------------------------- */
/* Test driver.                                                      */
/* ---------------------------------------------------------------- */

static const char *const *
blocks_for_case(const char *case_name)
{
    if (strncmp(case_name, "list_", 5) == 0) {
        return list_blocks;
    }
    if (strncmp(case_name, "array_", 6) == 0) {
        return array_blocks;
    }
    if (strncmp(case_name, "dict_", 5) == 0) {
        return dict_blocks;
    }
    return nullptr;
}

static bool
file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool
run_case(const drift_case_t *c, const char *stub_path,
         const char *prod_path)
{
    char *stub_out = nullptr;
    size_t stub_len = 0;
    char *stub_err = nullptr;

    if (!run_helper(stub_path, c->request, &stub_out, &stub_len,
                    &stub_err)) {
        fprintf(stderr,
                "FAIL %s: stub failed: %s\n",
                c->name, stub_err ? stub_err : "(no diagnostic)");
        free(stub_err);
        return false;
    }
    free(stub_err);

    /* Sanity: every helper response starts with `NCC_STATIC_INIT_OK`.
     * This is the load-bearing protocol invariant — drift here means
     * the stub stopped answering at all.
     */
    if (stub_len < 19u
        || memcmp(stub_out, "NCC_STATIC_INIT_OK ", 19u) != 0) {
        fprintf(stderr,
                "FAIL %s: stub response missing NCC_STATIC_INIT_OK\n",
                c->name);
        free(stub_out);
        return false;
    }

    if (!c->cross_helper || !prod_path || !file_exists(prod_path)) {
        if (c->cross_helper) {
            fprintf(stdout,
                    "SKIP %s: production helper unavailable at %s\n",
                    c->name, prod_path ? prod_path : "(no path)");
        }
        else {
            fprintf(stdout,
                    "PASS %s (stub-only sanity check)\n", c->name);
        }
        free(stub_out);
        return true;
    }

    char *prod_out = nullptr;
    size_t prod_len = 0;
    char *prod_err = nullptr;
    if (!run_helper(prod_path, c->request, &prod_out, &prod_len,
                    &prod_err)) {
        fprintf(stderr,
                "FAIL %s: production helper failed: %s\n",
                c->name, prod_err ? prod_err : "(no diagnostic)");
        free(prod_err);
        free(stub_out);
        return false;
    }
    free(prod_err);

    if (prod_len < 19u
        || memcmp(prod_out, "NCC_STATIC_INIT_OK ", 19u) != 0) {
        fprintf(stderr,
                "FAIL %s: production response missing NCC_STATIC_INIT_OK\n",
                c->name);
        free(stub_out);
        free(prod_out);
        return false;
    }

    /* Make NUL-terminated copies for normalization. */
    char *stub_norm = malloc(stub_len + 1);
    char *prod_norm = malloc(prod_len + 1);
    if (!stub_norm || !prod_norm) {
        fprintf(stderr, "FAIL %s: OOM allocating normalization buffers\n",
                c->name);
        free(stub_out);
        free(prod_out);
        free(stub_norm);
        free(prod_norm);
        return false;
    }
    memcpy(stub_norm, stub_out, stub_len);
    stub_norm[stub_len] = '\0';
    memcpy(prod_norm, prod_out, prod_len);
    prod_norm[prod_len] = '\0';

    normalize(stub_norm, c->symbol_prefix);
    normalize(prod_norm, c->symbol_prefix);

    bool   ok           = true;
    size_t block_count  = 0;
    const char *const *blocks = blocks_for_case(c->name);
    if (!blocks) {
        fprintf(stderr,
                "FAIL %s: no block map registered for case\n", c->name);
        ok = false;
    }
    for (size_t i = 0; ok && blocks[i]; i++) {
        char err_msg[1024];
        if (!compare_block(stub_norm, prod_norm, blocks[i],
                           err_msg, sizeof(err_msg))) {
            fprintf(stderr,
                    "FAIL %s: %s\n", c->name, err_msg);
            ok = false;
        }
        else {
            block_count++;
        }
    }

    if (ok) {
        fprintf(stdout,
                "PASS %s (cross-helper, %zu blocks)\n",
                c->name, block_count);
    }

    free(stub_out);
    free(prod_out);
    free(stub_norm);
    free(prod_norm);
    return ok;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <stub-path> [production-helper-path]\n",
                argv[0]);
        return 2;
    }
    const char *stub_path = argv[1];
    const char *prod_path = nullptr;
    if (argc >= 3 && argv[2][0] != '\0') {
        prod_path = argv[2];
    }
    else {
        const char *env = getenv("N00B_STATIC_INIT_HELPER");
        if (env && *env) {
            prod_path = env;
        }
        else {
            /* Documented default location for the canonical workspace
             * layout.  Absent files trigger the SKIP path.
             */
            prod_path =
                "/Users/viega/n00b/.workspaces/static-generated-objects/"
                "build/n00b-static-init-helper";
        }
    }

    bool all_ok = true;
    for (size_t i = 0;
         i < sizeof(drift_cases) / sizeof(drift_cases[0]); i++) {
        if (!run_case(&drift_cases[i], stub_path, prod_path)) {
            all_ok = false;
        }
    }

    if (!all_ok) {
        fprintf(stderr, "helper drift test FAILED\n");
        return 1;
    }
    fprintf(stdout, "helper drift test passed\n");
    return 0;
}
