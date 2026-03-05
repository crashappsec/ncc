// type_normalize.c — Type tree normalization + mangle + hash.
//
// Walks parse tree leaves, collects token text, applies normalization
// (drop restrict/_Atomic, sort qualifiers, strip attributes, canonical
// spacing), then hashes via SHA256 for typeid/typehash.

#include "util/type_normalize.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/string.h"
#include "util/sha256.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Leaf collection
// ============================================================================

typedef struct {
    const char **texts;
    size_t       count;
    size_t       cap;
} leaf_buf_t;

static void
leaf_buf_init(leaf_buf_t *lb)
{
    lb->cap   = 32;
    lb->count = 0;
    lb->texts = ncc_alloc_array(const char *, lb->cap);
}

static void
leaf_buf_push(leaf_buf_t *lb, const char *text)
{
    if (lb->count >= lb->cap) {
        lb->cap *= 2;
        lb->texts = ncc_realloc(lb->texts, lb->cap * sizeof(char *));
    }
    lb->texts[lb->count++] = text;
}

static void
leaf_buf_free(leaf_buf_t *lb)
{
    ncc_free(lb->texts);
}

static void
collect_leaves(ncc_parse_tree_t *node, leaf_buf_t *lb)
{
    if (!node) {
        return;
    }

    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            ncc_string_t s = ncc_option_get(tok->value);
            if (s.data && s.u8_bytes > 0) {
                leaf_buf_push(lb, s.data);
            }
        }
        return;
    }

    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        collect_leaves(ncc_tree_child(node, i), lb);
    }
}

// ============================================================================
// Normalization helpers
// ============================================================================

static bool
is_dropped_qualifier(const char *s)
{
    return strcmp(s, "restrict") == 0 || strcmp(s, "_Atomic") == 0
        || strcmp(s, "__restrict") == 0 || strcmp(s, "__restrict__") == 0;
}

static bool
is_kept_qualifier(const char *s)
{
    return strcmp(s, "const") == 0 || strcmp(s, "volatile") == 0
        || strcmp(s, "__const") == 0 || strcmp(s, "__const__") == 0
        || strcmp(s, "__volatile") == 0 || strcmp(s, "__volatile__") == 0;
}

// Normalize qualifier name to its canonical form.
static const char *
canonical_qualifier(const char *s)
{
    if (strcmp(s, "__const") == 0 || strcmp(s, "__const__") == 0) {
        return "const";
    }
    if (strcmp(s, "__volatile") == 0 || strcmp(s, "__volatile__") == 0) {
        return "volatile";
    }
    return s;
}

static bool
is_alnum_or_underscore(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static bool
is_attribute_start(const char *s)
{
    return strcmp(s, "__attribute__") == 0 || strcmp(s, "__attribute") == 0;
}


// ============================================================================
// Core normalization
// ============================================================================

ncc_string_t
ncc_normalize_type_tree(ncc_parse_tree_t *subtree)
{
    if (!subtree) {
        return ncc_string_empty();
    }

    leaf_buf_t lb;
    leaf_buf_init(&lb);
    collect_leaves(subtree, &lb);

    // Normalization pass over collected token texts.
    ncc_buffer_t *out = ncc_buffer_empty();

    // Qualifier accumulator (at most a few qualifiers).
    const char *quals[8];
    int         nquals     = 0;
    bool        last_alnum = false;

    // Skip attribute blocks: track depth for [[...]] and __attribute__((...)).
    int  attr_bracket_depth = 0;
    int  attr_paren_depth   = 0;
    bool in_attribute        = false;

    for (size_t i = 0; i < lb.count; i++) {
        const char *tok = lb.texts[i];

        // Handle [[...]] attribute blocks.
        if (attr_bracket_depth > 0) {
            if (strcmp(tok, "[") == 0) {
                attr_bracket_depth++;
            }
            else if (strcmp(tok, "]") == 0) {
                attr_bracket_depth--;
            }
            continue;
        }

        if (strcmp(tok, "[") == 0 && i + 1 < lb.count
            && strcmp(lb.texts[i + 1], "[") == 0) {
            attr_bracket_depth = 2;
            i++; // skip second [
            continue;
        }

        // Handle __attribute__((...)) blocks.
        if (in_attribute) {
            if (strcmp(tok, "(") == 0) {
                attr_paren_depth++;
            }
            else if (strcmp(tok, ")") == 0) {
                attr_paren_depth--;
                if (attr_paren_depth == 0) {
                    in_attribute = false;
                }
            }
            continue;
        }

        if (is_attribute_start(tok)) {
            in_attribute    = true;
            attr_paren_depth = 0;
            continue;
        }

        // Drop certain qualifiers.
        if (is_dropped_qualifier(tok)) {
            continue;
        }

        // Accumulate kept qualifiers for sorted output.
        if (is_kept_qualifier(tok)) {
            if (nquals < 8) {
                quals[nquals++] = canonical_qualifier(tok);
            }
            continue;
        }

        // Non-qualifier token: flush sorted qualifiers first.
        if (nquals > 0) {
            // Simple insertion sort for tiny array.
            for (int a = 1; a < nquals; a++) {
                const char *key = quals[a];
                int         b   = a - 1;
                while (b >= 0 && strcmp(quals[b], key) > 0) {
                    quals[b + 1] = quals[b];
                    b--;
                }
                quals[b + 1] = key;
            }

            for (int q = 0; q < nquals; q++) {
                if (out->byte_len > 0 && last_alnum) {
                    ncc_buffer_putc(out,' ');
                }
                ncc_buffer_puts(out,quals[q]);
                last_alnum = true;
            }
            nquals = 0;
        }

        // Emit token with canonical spacing.
        bool tok_is_alnum = tok[0] != '\0' && is_alnum_or_underscore(tok[0]);

        if (out->byte_len > 0 && tok_is_alnum && last_alnum) {
            ncc_buffer_putc(out,' ');
        }

        ncc_buffer_puts(out,tok);
        last_alnum = tok_is_alnum;
    }

    // Flush trailing qualifiers.
    if (nquals > 0) {
        for (int a = 1; a < nquals; a++) {
            const char *key = quals[a];
            int         b   = a - 1;
            while (b >= 0 && strcmp(quals[b], key) > 0) {
                quals[b + 1] = quals[b];
                b--;
            }
            quals[b + 1] = key;
        }

        for (int q = 0; q < nquals; q++) {
            if (out->byte_len > 0 && last_alnum) {
                ncc_buffer_putc(out,' ');
            }
            ncc_buffer_puts(out,quals[q]);
            last_alnum = true;
        }
    }

    leaf_buf_free(&lb);
    ncc_string_t result = ncc_string_from_raw(out->data, (int64_t)out->byte_len);
    ncc_free(out->data);
    ncc_free(out);
    return result;
}

// ============================================================================
// Mangle: SHA256 -> base64-like identifier
// ============================================================================

// clang-format off
static const signed char b64_map[] = {
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y',
    'z', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', -1, -2
};
// clang-format on

static char *
map_one(int bits, char *p)
{
    assert(bits < 64 && bits >= 0);

    int c = b64_map[bits];

    if (c > 0) {
        *p++ = (char)c;
    }
    else {
        *p++ = '_';
        *p++ = (char)('c' + c);
    }

    return p;
}

static void
digest_to_bytes(ncc_sha256_digest_t digest, uint8_t *out)
{
    for (int i = 0; i < NCC_SHA256_DIGEST_WORDS; i++) {
        uint32_t w     = digest[i];
        out[i * 4 + 0] = (uint8_t)(w >> 24);
        out[i * 4 + 1] = (uint8_t)(w >> 16);
        out[i * 4 + 2] = (uint8_t)(w >> 8);
        out[i * 4 + 3] = (uint8_t)(w);
    }
}

ncc_string_t
ncc_type_mangle(const char *normalized)
{
    ncc_sha256_digest_t digest;
    ncc_sha256_hash(normalized, strlen(normalized), digest);

    uint8_t dbytes[32];
    digest_to_bytes(digest, dbytes);

    // 30 bytes -> 40 base64 chars + 2 prefix + 1 NUL.
    // Some chars expand to 2 bytes (_a or _b), allocate extra.
    char buf[96];
    char *p = buf;
    *p++    = '_';
    *p++    = '_';

    for (int i = 0; i < 30;) {
        int c = dbytes[i++];
        int d = dbytes[i++];
        int e = dbytes[i++];

        p = map_one(c >> 2, p);
        p = map_one(((c & 0x3) << 4) | (d >> 4), p);
        p = map_one(((d & 0x0f) << 2) | (e >> 6), p);
        p = map_one(e & 0x3f, p);
    }

    *p = '\0';
    return ncc_string_from_raw(buf, (int64_t)(p - buf));
}

// ============================================================================
// Hash: SHA256 -> uint64
// ============================================================================

uint64_t
ncc_type_hash_u64(const char *normalized)
{
    ncc_sha256_digest_t digest;
    ncc_sha256_hash(normalized, strlen(normalized), digest);

    uint8_t dbytes[32];
    digest_to_bytes(digest, dbytes);

    uint64_t h = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | dbytes[i];
    }
    return h;
}
