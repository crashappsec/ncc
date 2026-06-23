// xform_rstr.c — Transform: r"..." rich string literals.
//
// Matches __ncc_rstr("...") calls (produced by the prescan in ncc.c) and
// replaces them with static compound literals containing pre-compiled
// styling data.
//
// Registered as post-order on "postfix_expression".

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "util/type_normalize.h"
#include "xform/xform_data.h"
#include "xform/xform_helpers.h"
#include "xform/xform_rstr.h"
#include "xform/xform_static_object.h"
#include "xform/xform_template.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// WP-011 Phase 5d: vendored XXH3_128bits so every r-string emission
// site can precompute the descriptor's `.cached_hash` slot.  Matches
// the runtime branch in `n00b_string_hash` (libn00b src/core/hash.c):
//
//   if (!s || !s->u8_bytes || !s->data) return n00b_hash_word(0ULL);
//   return n00b_xxh_convert(XXH3_128bits(s->data, s->u8_bytes));
//
// `n00b_xxh_convert` reinterprets `XXH128_hash_t{low64,high64}` as a
// 128-bit integer.  We emit the result as a `_BitInt(128)` literal
// expression and let the rstr template cast it through the
// descriptor's `.cached_hash` slot — same conversion as
// `compute_string_key_hash` in xform_array_literal.c.
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "vendor/xxhash.h"

// =========================================================================
// WP-011 Phase 5d: cached-hash precomputation for the descriptor's
// `.cached_hash` slot.
// =========================================================================
//
// Every r-string emission (standalone, list element, struct field,
// dict key, ...) now populates the descriptor's `cached_hash` slot
// with the XXH3_128bits of the post-rich-markup UTF-8 content,
// mirroring `n00b_string_hash` exactly.  Runtime `n00b_hash()`
// short-circuits on this slot (D-066), so content-equal r-strings
// produce content-equal hashes regardless of which call site emitted
// them.
//
// Format: `(((n00b_uint128_t)0xHIULL << 64) | (n00b_uint128_t)0xLOULL)`.
// The descriptor template casts this through `n00b_uint128_t` so the
// shift is well-defined (both halves are cast to 128 bits before the
// shift / OR).  The empty-string case (`content_len == 0`) emits "0"
// — `n00b_string_hash` falls back to `n00b_hash_word(0ULL)` for empty
// inputs, which we cannot reproduce at ncc compile time without
// pulling in libn00b's `n00b_word_t` layout.  Empty r-string
// emissions are pre-Phase-5d behavior (cached_hash unset), so leaving
// them at 0 preserves the prior short-circuit semantics — a runtime
// `n00b_hash(rstr)` on an empty r-string still falls through to the
// vtable, which then takes the `n00b_hash_word(0ULL)` branch.
//
// Returned buffer is owned by the caller (free with `ncc_free`).
static char *
format_rstr_cached_hash_expr(const char *content, size_t content_len) {
  if (!content || content_len == 0) {
    char *out = ncc_alloc_size(1, 2);
    out[0] = '0';
    out[1] = '\0';
    return out;
  }

  XXH128_hash_t h = XXH3_128bits(content, content_len);

  // Buffer sized to hold `(((n00b_uint128_t)0xHHHHHHHHHHHHHHHHULL <<
  // 64) | (n00b_uint128_t)0xLLLLLLLLLLLLLLLLULL)` plus terminator.
  char *out = ncc_alloc_size(1, 96);
  snprintf(out, 96,
           "(((n00b_uint128_t)0x%016llxULL << 64)"
           "|(n00b_uint128_t)0x%016llxULL)",
           (unsigned long long)h.high64,
           (unsigned long long)h.low64);
  return out;
}

// =========================================================================
// Access to ncc_xform_data_t fields
// =========================================================================

static ncc_template_registry_t *get_template_reg(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->template_reg;
}

static const char *get_rstr_string_type(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_string_type;
}

static const char *get_rstr_text_style_type(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_text_style_type;
}

static const char *get_rstr_style_record_type(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_style_record_type;
}

static const char *get_rstr_static_ref_template_styled(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_static_ref_template_styled;
}

static const char *get_rstr_static_ref_template_plain(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_static_ref_template_plain;
}

static const char *get_rstr_static_ref_expr_styled(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_static_ref_expr_styled;
}

static const char *get_rstr_static_ref_expr_plain(ncc_xform_ctx_t *ctx) {
  return ncc_xform_get_data(ctx)->rstr_static_ref_expr_plain;
}

static void require_rstr_template_slots(const char *name, int requested,
                                        int available) {
  if (requested <= available) {
    return;
  }

  fprintf(stderr,
          "ncc: error: r-string template '%s' references slot $%d, but only "
          "$0..$%d are available\n",
          name, requested - 1, available - 1);
  exit(1);
}

// =========================================================================
// UTF-8 codepoint counter
// =========================================================================

static int64_t count_utf8_codepoints(const char *data, int64_t len) {
  int64_t count = 0;

  for (int64_t i = 0; i < len; i++) {
    if ((data[i] & 0xC0) != 0x80) {
      count++;
    }
  }

  return count;
}

// =========================================================================
// Rich markup property / case tables
// =========================================================================

static const struct {
  const char *name;
  const char *field_name;
} prop_table[] = {
    {"b", "bold"},
    {"bold", "bold"},
    {"i", "italic"},
    {"italic", "italic"},
    {"u", "underline"},
    {"underline", "underline"},
    {"uu", "double_underline"},
    {"2u", "double_underline"},
    {"st", "strikethrough"},
    {"strike", "strikethrough"},
    {"strikethrough", "strikethrough"},
    {"r", "reverse"},
    {"reverse", "reverse"},
    {"dim", "dim"},
    {"faint", "dim"},
    {"blink", "blink"},
    {nullptr, nullptr},
};

typedef enum {
  RSTR_CASE_NONE = 0,
  RSTR_CASE_UPPER = 1,
  RSTR_CASE_LOWER = 2,
  RSTR_CASE_TITLE = 3,
  RSTR_CASE_CAPS = 4,
} rstr_case_t;

static const struct {
  const char *name;
  rstr_case_t value;
} case_table[] = {
    {"upper", RSTR_CASE_UPPER},
    {"up", RSTR_CASE_UPPER},
    {"lower", RSTR_CASE_LOWER},
    {"l", RSTR_CASE_LOWER},
    {"caps", RSTR_CASE_CAPS},
    {"allcaps", RSTR_CASE_CAPS},
    {"t", RSTR_CASE_TITLE},
    {"title", RSTR_CASE_TITLE},
    {nullptr, 0},
};

static const char *lookup_prop_field(const char *name, int name_len) {
  for (int i = 0; prop_table[i].name; i++) {
    if ((int)strlen(prop_table[i].name) == name_len &&
        memcmp(prop_table[i].name, name, name_len) == 0) {
      return prop_table[i].field_name;
    }
  }
  return nullptr;
}

static int lookup_case_val(const char *name, int name_len) {
  for (int i = 0; case_table[i].name; i++) {
    if ((int)strlen(case_table[i].name) == name_len &&
        memcmp(case_table[i].name, name, name_len) == 0) {
      return (int)case_table[i].value;
    }
  }
  return -1;
}

// =========================================================================
// Segment list (compile-time parse result)
// =========================================================================

typedef enum {
  SEG_TEXT,
  SEG_PROP_ON,
  SEG_PROP_OFF,
  SEG_CASE_ON,
  SEG_CASE_OFF,
  SEG_STYLE_ON,
  SEG_STYLE_OFF,
  SEG_ROLE_ON,
  SEG_ROLE_OFF,
  SEG_RESET,
} seg_kind_t;

typedef struct {
  seg_kind_t kind;
  int text_offset;
  int text_length;
  const char *field_name;
  int case_val;
} rstr_segment_t;

typedef struct {
  rstr_segment_t *segs;
  int count;
  int cap;
} rstr_seg_list_t;

static void rseg_push(rstr_seg_list_t *sl, rstr_segment_t seg) {
  if (sl->count >= sl->cap) {
    int new_cap = sl->cap ? sl->cap * 2 : 16;
    rstr_segment_t *new_s = ncc_alloc_array(rstr_segment_t, (size_t)new_cap);

    if (sl->segs) {
      memcpy(new_s, sl->segs, (size_t)sl->count * sizeof(rstr_segment_t));
      ncc_free(sl->segs);
    }

    sl->segs = new_s;
    sl->cap = new_cap;
  }

  sl->segs[sl->count++] = seg;
}


// =========================================================================
// Markup parser
// =========================================================================

static void classify_tag(rstr_seg_list_t *sl, const char *tag_body,
                         int tag_len) {
  if (tag_len == 0) {
    return;
  }

  // Reset: "/" alone.
  if (tag_len == 1 && tag_body[0] == '/') {
    rseg_push(sl, (rstr_segment_t){.kind = SEG_RESET});
    return;
  }

  // Substitution: starts with '#' — compile error.
  if (tag_body[0] == '#') {
    fprintf(stderr, "ncc: error: r\"...\" string literals cannot contain "
                    "substitutions (#)\n");
    exit(1);
  }

  bool is_close = (tag_body[0] == '/');
  const char *name = is_close ? tag_body + 1 : tag_body;
  int name_len = is_close ? tag_len - 1 : tag_len;

  if (name_len == 0) {
    return;
  }

  // Role: starts with '@'.
  if (name[0] == '@') {
    char *tag_name = ncc_alloc_size(1, (size_t)(name_len + 1));

    memcpy(tag_name, name, (size_t)name_len);
    tag_name[name_len] = '\0';
    rseg_push(sl, (rstr_segment_t){
                      .kind = is_close ? SEG_ROLE_OFF : SEG_ROLE_ON,
                      .field_name = tag_name,
                  });
    return;
  }

  // Inline property?
  const char *field = lookup_prop_field(name, name_len);

  if (field) {
    rseg_push(sl, (rstr_segment_t){
                      .kind = is_close ? SEG_PROP_OFF : SEG_PROP_ON,
                      .field_name = field,
                  });
    return;
  }

  // Text case?
  int cval = lookup_case_val(name, name_len);

  if (cval >= 0) {
    rseg_push(sl, (rstr_segment_t){
                      .kind = is_close ? SEG_CASE_OFF : SEG_CASE_ON,
                      .case_val = cval,
                  });
    return;
  }

  // Named style (default).
  char *tag_name = ncc_alloc_size(1, (size_t)(name_len + 1));

  memcpy(tag_name, name, (size_t)name_len);
  tag_name[name_len] = '\0';
  rseg_push(sl, (rstr_segment_t){
                    .kind = is_close ? SEG_STYLE_OFF : SEG_STYLE_ON,
                    .field_name = tag_name,
                });
}

static void parse_rich_markup(const char *desc, int desc_len,
                              rstr_seg_list_t *sl, ncc_buffer_t *tb) {
  int i = 0;
  int text_start = 0;

  while (i < desc_len) {
    // Escape: backslash.
    if (desc[i] == '\\' && i + 1 < desc_len) {
      char next = desc[i + 1];

      // \xC2\xAB = «, \xC2\xBB = »
      if ((unsigned char)next == 0xC2 && i + 2 < desc_len) {
        unsigned char after = (unsigned char)desc[i + 2];

        if (after == 0xAB || after == 0xBB) {
          // Flush text before backslash.
          if (i > text_start) {
            int off = tb->byte_len;

            ncc_buffer_append(tb,desc + text_start, i - text_start);
            rseg_push(sl, (rstr_segment_t){
                              .kind = SEG_TEXT,
                              .text_offset = off,
                              .text_length = i - text_start,
                          });
          }

          // Emit the escaped guillemet.
          int off = tb->byte_len;

          ncc_buffer_append(tb,desc + i + 1, 2);
          rseg_push(sl, (rstr_segment_t){
                            .kind = SEG_TEXT,
                            .text_offset = off,
                            .text_length = 2,
                        });
          i += 3;
          text_start = i;
          continue;
        }
      }

      // Normal C escapes: pass through as-is.
      i += 2;
      continue;
    }

    // Guillemet tag: « ... »
    if (i + 1 < desc_len && (unsigned char)desc[i] == 0xC2 &&
        (unsigned char)desc[i + 1] == 0xAB) {
      if (i > text_start) {
        int off = tb->byte_len;

        ncc_buffer_append(tb,desc + text_start, i - text_start);
        rseg_push(sl, (rstr_segment_t){
                          .kind = SEG_TEXT,
                          .text_offset = off,
                          .text_length = i - text_start,
                      });
      }

      int tag_start = i + 2;
      int j = tag_start;

      while (j + 1 < desc_len && !((unsigned char)desc[j] == 0xC2 &&
                                   (unsigned char)desc[j + 1] == 0xBB)) {
        j++;
      }

      if (j + 1 < desc_len) {
        classify_tag(sl, desc + tag_start, j - tag_start);
        i = j + 2;
        text_start = i;
      } else {
        i += 2;
      }
      continue;
    }

    // Bracket tag: [| ... |]
    if (desc[i] == '[' && i + 1 < desc_len && desc[i + 1] == '|') {
      if (i > text_start) {
        int off = tb->byte_len;

        ncc_buffer_append(tb,desc + text_start, i - text_start);
        rseg_push(sl, (rstr_segment_t){
                          .kind = SEG_TEXT,
                          .text_offset = off,
                          .text_length = i - text_start,
                      });
      }

      int tag_start = i + 2;
      int j = tag_start;

      while (j + 1 < desc_len && !(desc[j] == '|' && desc[j + 1] == ']')) {
        j++;
      }

      if (j + 1 < desc_len) {
        classify_tag(sl, desc + tag_start, j - tag_start);
        i = j + 2;
        text_start = i;
      } else {
        fprintf(stderr, "ncc: error: unclosed '[|' tag in r-string\n");
        exit(1);
      }
      continue;
    }

    i++;
  }

  // Trailing text.
  if (i > text_start) {
    int off = tb->byte_len;

    ncc_buffer_append(tb,desc + text_start, i - text_start);
    rseg_push(sl, (rstr_segment_t){
                      .kind = SEG_TEXT,
                      .text_offset = off,
                      .text_length = i - text_start,
                  });
  }
}

// =========================================================================
// Style record builder
// =========================================================================

typedef struct {
  enum {
    PSTYLE_PROP,
    PSTYLE_CASE,
    PSTYLE_NAMED,
    PSTYLE_ROLE,
  } kind;
  const char *field_name;
  int case_val;
  int start_byte;
} pending_style_t;

typedef struct {
  pending_style_t *items;
  int count;
  int cap;
} pending_stack_t;

static void pending_push(pending_stack_t *ps, pending_style_t item) {
  if (ps->count >= ps->cap) {
    int new_cap = ps->cap ? ps->cap * 2 : 16;
    pending_style_t *np = ncc_alloc_array(pending_style_t, (size_t)new_cap);

    if (ps->items) {
      memcpy(np, ps->items, (size_t)ps->count * sizeof(pending_style_t));
      ncc_free(ps->items);
    }

    ps->items = np;
    ps->cap = new_cap;
  }

  ps->items[ps->count++] = item;
}

typedef struct {
  int start;
  int end;
  const char *field_name;
  int kind;
  int case_val;
} out_style_t;

typedef struct {
  out_style_t *items;
  int count;
  int cap;
} out_style_list_t;

static void out_push(out_style_list_t *ol, out_style_t item) {
  if (ol->count >= ol->cap) {
    int new_cap = ol->cap ? ol->cap * 2 : 16;
    out_style_t *np = ncc_alloc_array(out_style_t, (size_t)new_cap);

    if (ol->items) {
      memcpy(np, ol->items, (size_t)ol->count * sizeof(out_style_t));
      ncc_free(ol->items);
    }

    ol->items = np;
    ol->cap = new_cap;
  }

  ol->items[ol->count++] = item;
}

static void build_style_records(rstr_seg_list_t *sl, out_style_list_t *out) {
  pending_stack_t ps = {0};
  int text_pos = 0;

  for (int i = 0; i < sl->count; i++) {
    rstr_segment_t *seg = &sl->segs[i];

    switch (seg->kind) {
    case SEG_TEXT:
      text_pos = seg->text_offset + seg->text_length;
      break;

    case SEG_PROP_ON:
      pending_push(&ps, (pending_style_t){
                            .kind = PSTYLE_PROP,
                            .field_name = seg->field_name,
                            .start_byte = text_pos,
                        });
      break;

    case SEG_PROP_OFF:
      for (int j = ps.count - 1; j >= 0; j--) {
        if (ps.items[j].kind == PSTYLE_PROP &&
            strcmp(ps.items[j].field_name, seg->field_name) == 0) {
          out_push(out, (out_style_t){
                            .start = ps.items[j].start_byte,
                            .end = text_pos,
                            .field_name = ps.items[j].field_name,
                            .kind = PSTYLE_PROP,
                        });

          for (int k = j; k < ps.count - 1; k++) {
            ps.items[k] = ps.items[k + 1];
          }

          ps.count--;
          break;
        }
      }
      break;

    case SEG_CASE_ON:
      pending_push(&ps, (pending_style_t){
                            .kind = PSTYLE_CASE,
                            .case_val = seg->case_val,
                            .start_byte = text_pos,
                        });
      break;

    case SEG_CASE_OFF:
      for (int j = ps.count - 1; j >= 0; j--) {
        if (ps.items[j].kind == PSTYLE_CASE) {
          out_push(out, (out_style_t){
                            .start = ps.items[j].start_byte,
                            .end = text_pos,
                            .kind = PSTYLE_CASE,
                            .case_val = ps.items[j].case_val,
                        });

          for (int k = j; k < ps.count - 1; k++) {
            ps.items[k] = ps.items[k + 1];
          }

          ps.count--;
          break;
        }
      }
      break;

    case SEG_STYLE_ON:
      pending_push(&ps, (pending_style_t){
                            .kind = PSTYLE_NAMED,
                            .field_name = seg->field_name,
                            .start_byte = text_pos,
                        });
      break;

    case SEG_STYLE_OFF:
      for (int j = ps.count - 1; j >= 0; j--) {
        if (ps.items[j].kind == PSTYLE_NAMED &&
            strcmp(ps.items[j].field_name, seg->field_name) == 0) {
          out_push(out, (out_style_t){
                            .start = ps.items[j].start_byte,
                            .end = text_pos,
                            .field_name = ps.items[j].field_name,
                            .kind = PSTYLE_NAMED,
                        });

          for (int k = j; k < ps.count - 1; k++) {
            ps.items[k] = ps.items[k + 1];
          }

          ps.count--;
          break;
        }
      }
      break;

    case SEG_ROLE_ON:
      pending_push(&ps, (pending_style_t){
                            .kind = PSTYLE_ROLE,
                            .field_name = seg->field_name,
                            .start_byte = text_pos,
                        });
      break;

    case SEG_ROLE_OFF:
      for (int j = ps.count - 1; j >= 0; j--) {
        if (ps.items[j].kind == PSTYLE_ROLE &&
            strcmp(ps.items[j].field_name, seg->field_name) == 0) {
          out_push(out, (out_style_t){
                            .start = ps.items[j].start_byte,
                            .end = text_pos,
                            .field_name = ps.items[j].field_name,
                            .kind = PSTYLE_ROLE,
                        });

          for (int k = j; k < ps.count - 1; k++) {
            ps.items[k] = ps.items[k + 1];
          }

          ps.count--;
          break;
        }
      }
      break;

    case SEG_RESET:
      for (int j = 0; j < ps.count; j++) {
        out_push(out, (out_style_t){
                          .start = ps.items[j].start_byte,
                          .end = text_pos,
                          .field_name = ps.items[j].field_name,
                          .kind = ps.items[j].kind,
                          .case_val = ps.items[j].case_val,
                      });
      }
      ps.count = 0;
      break;
    }
  }

  // Close any remaining open styles (extend to end of string).
  for (int j = 0; j < ps.count; j++) {
    out_push(out, (out_style_t){
                      .start = ps.items[j].start_byte,
                      .end = -1,
                      .field_name = ps.items[j].field_name,
                      .kind = ps.items[j].kind,
                      .case_val = ps.items[j].case_val,
                  });
  }

  ncc_free(ps.items);
}

// =========================================================================
// Code emitter
// =========================================================================

static void emit_escaped_string(ncc_buffer_t *buf, const char *data, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];

    switch (c) {
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
    case '\0':
      ncc_buffer_puts(buf, "\\0");
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        ncc_buffer_printf(buf, "\\x%02x", c);
      } else {
        ncc_buffer_putc(buf, (char)c);
      }
      break;
    }
  }
}

static void emit_style_var(ncc_buffer_t *buf, out_style_t *os, int idx,
                           int uid, const char *text_style_type) {
  if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
    // Deferred: info = nullptr, tag = "name". No style variable needed.
  } else if (os->kind == PSTYLE_CASE) {
    ncc_buffer_printf(buf,
                      "static %s _ncc_rs_%d_ts_%d="
                      "{.text_case=%d};",
                      text_style_type, uid, idx, os->case_val);
  } else {
    ncc_buffer_printf(buf,
                      "static %s _ncc_rs_%d_ts_%d="
                      "{.%s=2};",
                      text_style_type, uid, idx, os->field_name);
  }
}

// Emit style variable declarations and style info struct as a string.
// This becomes the $0 argument to the rstr_styled template.
// Returns an allocated string (caller frees).
static char *emit_style_declarations(ncc_xform_ctx_t *ctx,
                                     out_style_list_t *out, int uid) {
  ncc_buffer_t *buf = ncc_buffer_empty();
  const char *text_style_type   = get_rstr_text_style_type(ctx);
  const char *style_record_type = get_rstr_style_record_type(ctx);

  for (int i = 0; i < out->count; i++) {
    emit_style_var(buf, &out->items[i], i, uid, text_style_type);
  }

  ncc_buffer_printf(buf,
                    "static struct{"
                    "int64_t num_styles;"
                    "%s*base_style;"
                    "%s styles[%d];"
                    "}_ncc_rs_%d_si={",
                    text_style_type, style_record_type, out->count, uid);
  ncc_buffer_printf(buf, ".num_styles=%d,.base_style=((%s*)0),",
                    out->count, text_style_type);
  ncc_buffer_puts(buf, ".styles={");

  for (int i = 0; i < out->count; i++) {
    if (i > 0) {
      ncc_buffer_putc(buf, ',');
    }

    out_style_t *os = &out->items[i];

    ncc_buffer_putc(buf, '{');

    if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
      ncc_buffer_printf(buf, ".info=((%s*)0),.tag=\"", text_style_type);
      emit_escaped_string(buf, os->field_name, (int)strlen(os->field_name));
      ncc_buffer_putc(buf, '"');
    } else {
      ncc_buffer_printf(buf, ".info=&_ncc_rs_%d_ts_%d,.tag=((void*)0)", uid,
                        i);
    }

    ncc_buffer_printf(buf, ",.start=%d", os->start);

    if (os->end >= 0) {
      ncc_buffer_printf(buf, ",.end={.has_value=1,.value=(size_t)%d}",
                        os->end);
    } else {
      ncc_buffer_puts(buf, ",.end={.has_value=0}");
    }

    ncc_buffer_putc(buf, '}');
  }

  ncc_buffer_puts(buf, "}};");
  return ncc_buffer_take(buf);
}

// =========================================================================
// String literal extraction
// =========================================================================

static char *extract_rstr_content(ncc_parse_tree_t *arglist, int *out_len) {
  ncc_buffer_t *buf = ncc_buffer_empty();

  // DFS to find all STRING token leaves.
  typedef struct {
    ncc_parse_tree_t **items;
    int count;
    int cap;
  } node_stack_t;

  node_stack_t stack = {0};

  stack.cap = 32;
  stack.items = ncc_alloc_array(ncc_parse_tree_t *, (size_t)stack.cap);
  stack.items[stack.count++] = arglist;

  while (stack.count > 0) {
    ncc_parse_tree_t *node = stack.items[--stack.count];

    if (!node) {
      continue;
    }

    if (ncc_tree_is_leaf(node)) {
      const char *text = ncc_xform_leaf_text(node);

      if (!text) {
        continue;
      }

      size_t tlen = strlen(text);

      if (tlen >= 2 && text[0] == '"' && text[tlen - 1] == '"') {
        const char *p = text + 1;
        const char *end = text + tlen - 1;

        while (p < end) {
          if (*p == '\\' && p + 1 < end) {
            p++;

            switch (*p) {
            case 'n':
              ncc_buffer_putc(buf, '\n');
              p++;
              break;
            case 'r':
              ncc_buffer_putc(buf, '\r');
              p++;
              break;
            case 't':
              ncc_buffer_putc(buf, '\t');
              p++;
              break;
            case '0':
              ncc_buffer_putc(buf, '\0');
              p++;
              break;
            case '\\':
              ncc_buffer_putc(buf, '\\');
              p++;
              break;
            case '"':
              ncc_buffer_putc(buf, '"');
              p++;
              break;
            case 'a':
              ncc_buffer_putc(buf, '\a');
              p++;
              break;
            case 'b':
              ncc_buffer_putc(buf, '\b');
              p++;
              break;
            case 'f':
              ncc_buffer_putc(buf, '\f');
              p++;
              break;
            case 'v':
              ncc_buffer_putc(buf, '\v');
              p++;
              break;
            case 'x': {
              p++;
              unsigned int val = 0;

              for (int d = 0; d < 2 && p < end; d++, p++) {
                unsigned char c = (unsigned char)*p;

                if (c >= '0' && c <= '9') {
                  val = val * 16 + (c - '0');
                } else if (c >= 'a' && c <= 'f') {
                  val = val * 16 + (c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                  val = val * 16 + (c - 'A' + 10);
                } else {
                  break;
                }
              }

              ncc_buffer_putc(buf, (char)val);
              break;
            }
            default:
              ncc_buffer_putc(buf, '\\');
              ncc_buffer_putc(buf, *p);
              p++;
              break;
            }
          } else {
            ncc_buffer_putc(buf, *p);
            p++;
          }
        }
      }
    } else {
      // Push children in reverse for in-order traversal.
      size_t nc = ncc_tree_num_children(node);

      for (int i = (int)nc - 1; i >= 0; i--) {
        ncc_parse_tree_t *kid = ncc_tree_child(node, (size_t)i);

        if (!kid) {
          continue;
        }

        if (stack.count >= stack.cap) {
          stack.cap *= 2;
          stack.items = ncc_realloc(
              stack.items, (size_t)stack.cap * sizeof(ncc_parse_tree_t *));
        }

        stack.items[stack.count++] = kid;
      }
    }
  }

  ncc_free(stack.items);
  *out_len = (int)buf->byte_len;
  return ncc_buffer_take(buf);
}

// =========================================================================
// Callee name helper
// =========================================================================

static const char *rstr_get_callee_name(ncc_parse_tree_t *node) {
  // child[0] is the callee.
  if (ncc_tree_num_children(node) == 0) {
    return nullptr;
  }

  ncc_parse_tree_t *callee = ncc_tree_child(node, 0);

  if (!callee) {
    return nullptr;
  }

  // Walk down to find the first leaf token.
  while (callee && !ncc_tree_is_leaf(callee)) {
    if (ncc_tree_num_children(callee) == 0) {
      return nullptr;
    }
    callee = ncc_tree_child(callee, 0);
  }

  return ncc_xform_leaf_text(callee);
}

static ncc_parse_tree_t *find_rstr_call(ncc_parse_tree_t *node) {
  if (!node) {
    return nullptr;
  }

  if (!ncc_tree_is_leaf(node) && ncc_xform_nt_name_is(node, "postfix_expression")) {
    size_t nc = ncc_tree_num_children(node);
    if (nc >= 3 && ncc_xform_leaf_text_eq(ncc_tree_child(node, 1), "(")) {
      const char *name = rstr_get_callee_name(node);
      if (name && strcmp(name, "__ncc_rstr") == 0) {
        return node;
      }
    }
  }

  if (ncc_tree_is_leaf(node)) {
    return nullptr;
  }

  size_t nc = ncc_tree_num_children(node);
  for (size_t i = 0; i < nc; i++) {
    ncc_parse_tree_t *found = find_rstr_call(ncc_tree_child(node, i));
    if (found) {
      return found;
    }
  }

  return nullptr;
}

ncc_rstr_static_ref_t ncc_rstr_build_static_ref(ncc_xform_ctx_t *ctx,
                                                ncc_parse_tree_t *node) {
  // WP-011 Phase 5d: the default entry point now delegates to the
  // `_ex` variant with `nullptr`, which causes `_ex` to compute the
  // descriptor's `.cached_hash` slot from the post-rich-markup UTF-8
  // bytes (matching `n00b_string_hash` exactly).  Callers no longer
  // need to thread a precomputed expression through the default
  // path — every emission gets the content hash.
  return ncc_rstr_build_static_ref_ex(ctx, node, nullptr);
}

ncc_rstr_static_ref_t ncc_rstr_build_static_ref_ex(
    ncc_xform_ctx_t *ctx,
    ncc_parse_tree_t *node,
    const char *cached_hash_expr) {
  ncc_parse_tree_t *call = find_rstr_call(node);
  if (!call) {
    return (ncc_rstr_static_ref_t){0};
  }

  ncc_parse_tree_t *kid2 = ncc_tree_child(call, 2);
  if (!kid2 || ncc_xform_leaf_text_eq(kid2, ")")) {
    fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
    exit(1);
  }

  int content_len;
  char *content = extract_rstr_content(kid2, &content_len);
  if (!content) {
    fprintf(stderr,
            "ncc: error: __ncc_rstr() argument must be a string literal\n");
    exit(1);
  }

  rstr_seg_list_t sl = {0};
  ncc_buffer_t *tb = ncc_buffer_empty();
  parse_rich_markup(content, content_len, &sl, tb);

  // WP-011 Phase 5d: if the caller did not supply a precomputed
  // cached_hash expression (the dict-key path does — see
  // xform_array_literal.c), derive one from the post-rich-markup
  // bytes here so every emitted descriptor carries the same XXH3
  // content hash a runtime `n00b_string_hash` would compute.  The
  // dict-key path's explicit expression takes precedence and is
  // bit-identical (same algorithm over the same bytes).
  char *computed_cached_hash = nullptr;
  const char *effective_cached_hash_expr = cached_hash_expr;
  if (!effective_cached_hash_expr || !*effective_cached_hash_expr) {
    computed_cached_hash =
        format_rstr_cached_hash_expr(tb->data, tb->byte_len);
    effective_cached_hash_expr = computed_cached_hash;
  }

  out_style_list_t out = {0};
  build_style_records(&sl, &out);
  int uid = ctx->unique_id++;

  char var_name[64];
  snprintf(var_name, sizeof(var_name), "_ncc_rs_%d", uid);

  int64_t cp_count = count_utf8_codepoints(tb->data, (int64_t)tb->byte_len);

  ncc_buffer_t *data_buf = ncc_buffer_empty();
  ncc_buffer_putc(data_buf, '"');
  emit_escaped_string(data_buf, tb->data, (int)tb->byte_len);
  ncc_buffer_putc(data_buf, '"');
  char *data_str = ncc_buffer_take(data_buf);

  char bytes_str[32];
  snprintf(bytes_str, sizeof(bytes_str), "%zu", tb->byte_len);

  char cp_str[32];
  snprintf(cp_str, sizeof(cp_str), "%lld", (long long)cp_count);

  // Hash the POINTER form: the descriptor's tinfo must match the runtime
  // type registry (keyed by typehash(T*)) so a static r-string resolves to
  // the same n00b_string_t type_info as a heap string. get_rstr_string_type
  // is configured as a value type (n00b's array-literal type matcher relies
  // on that), so use the '*'-normalizing helper here.
  char *typehash_str = ncc_static_object_ptr_typehash_expr(get_rstr_string_type(ctx));

  char wrapper_var[64];
  snprintf(wrapper_var, sizeof(wrapper_var), "_ncc_rsh_%d", uid);

  ncc_static_object_names_t names;
  ncc_static_object_names_for_rstr(&names, uid);

  ncc_static_object_slots_t stobj;
  ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2", "1",
                               "nullptr", "nullptr",
                               "N00B_STATIC_IDENTITY_NCC_RSTR", "ncc-rstr",
                               call, bytes_str);

  char *decl_str = nullptr;
  char *expr_str = nullptr;

  if (out.count > 0) {
    char *style_decls = emit_style_declarations(ctx, &out, uid);

    char styling_str[64];
    snprintf(styling_str, sizeof(styling_str), "&_ncc_rs_%d_si", uid);

    // Slot layout mirrors the main styled r-string template:
    // $0=style_decls $1=var $2=bytes $3=data $4=codepoints
    // $5=styling $6=typehash $7=wrapper_var
    // $8=descriptor $9=entry $10=object_id $11=flags
    // $12=scan_kind $13=scan_cb $14=scan_user $15=entry_attr
    // $16=identity_decl $17=identity_expr
    // $18=cached_hash (WP-011 Phase 5d — descriptor cached_hash
    //                  expression; XXH3_128bits over the post-rich-
    //                  markup UTF-8 content for every emission).
    const char *all_args[] = {
        style_decls, var_name, bytes_str, data_str,
        cp_str,      styling_str, typehash_str, wrapper_var,
        stobj.desc_name,    stobj.entry_name, stobj.object_id, stobj.flags,
        stobj.scan_kind,    stobj.scan_cb,    stobj.scan_user, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
        effective_cached_hash_expr,
    };

    decl_str = ncc_static_object_expand_template(
        "r-string static-ref",
        get_rstr_static_ref_template_styled(ctx), all_args, 19);
    expr_str = ncc_static_object_expand_template(
        "r-string static-ref expression",
        get_rstr_static_ref_expr_styled(ctx), all_args, 19);
    ncc_free(style_decls);
  } else {
    // Slot layout mirrors the main plain r-string template:
    // $0=var $1=bytes $2=data $3=codepoints $4=typehash $5=wrapper_var
    // $6=descriptor $7=entry $8=object_id $9=flags
    // $10=scan_kind $11=scan_cb $12=scan_user $13=entry_attr
    // $14=identity_decl $15=identity_expr
    // $16=cached_hash (WP-011 Phase 5d — descriptor cached_hash
    //                  expression; XXH3_128bits over the post-rich-
    //                  markup UTF-8 content for every emission).
    const char *all_args[] = {
        var_name, bytes_str, data_str, cp_str, typehash_str, wrapper_var,
        stobj.desc_name, stobj.entry_name, stobj.object_id, stobj.flags,
        stobj.scan_kind, stobj.scan_cb,    stobj.scan_user, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
        effective_cached_hash_expr,
    };

    decl_str = ncc_static_object_expand_template(
        "r-string static-ref",
        get_rstr_static_ref_template_plain(ctx), all_args, 17);
    expr_str = ncc_static_object_expand_template(
        "r-string static-ref expression",
        get_rstr_static_ref_expr_plain(ctx), all_args, 17);
  }

  // WP-011 Phase 3c.ii.a: copy out the post-rich-markup UTF-8 content
  // so callers (the dict-key path) can hash it via the same
  // XXH3_128bits sequence `n00b_string_hash` uses at runtime
  // (`XXH3_128bits(s->data, s->u8_bytes)`).  The buffer is the bytes
  // backing the static `ncc_string_t`'s `.data` field — same sequence,
  // copied here so the caller doesn't need to depend on `tb`'s
  // lifetime.  Returned via the result struct; caller owns the
  // allocation.
  char  *content_copy = nullptr;
  size_t copy_len     = tb->byte_len;
  if (copy_len > 0) {
      content_copy = ncc_alloc_size(1, copy_len);
      memcpy(content_copy, tb->data, copy_len);
  }

  ncc_static_object_slots_cleanup(&stobj);
  ncc_free(content);
  ncc_free(data_str);
  ncc_free(typehash_str);
  ncc_free(sl.segs);
  ncc_buffer_free(tb);
  ncc_free(out.items);
  if (computed_cached_hash) {
    ncc_free(computed_cached_hash);
  }

  return (ncc_rstr_static_ref_t){
      .decl        = decl_str,
      .expr        = expr_str,
      .content     = content_copy,
      .content_len = copy_len,
  };
}

ncc_rstr_managed_expr_t
ncc_rstr_build_plain_managed_expr(ncc_parse_tree_t *node)
{
  ncc_parse_tree_t *call = find_rstr_call(node);
  if (!call) {
    return (ncc_rstr_managed_expr_t){0};
  }

  ncc_parse_tree_t *kid2 = ncc_tree_child(call, 2);
  if (!kid2 || ncc_xform_leaf_text_eq(kid2, ")")) {
    fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
    exit(1);
  }

  int content_len;
  char *content = extract_rstr_content(kid2, &content_len);
  if (!content) {
    fprintf(stderr,
            "ncc: error: __ncc_rstr() argument must be a string literal\n");
    exit(1);
  }

  rstr_seg_list_t sl = {0};
  ncc_buffer_t *tb = ncc_buffer_empty();
  parse_rich_markup(content, content_len, &sl, tb);

  out_style_list_t out = {0};
  build_style_records(&sl, &out);

  ncc_buffer_t *expr = nullptr;
  if (out.count == 0) {
    expr = ncc_buffer_empty();
    ncc_buffer_puts(expr, "n00b_ncc_rstr(\"");
    emit_escaped_string(expr, tb->data, (int)tb->byte_len);
    ncc_buffer_puts(expr, "\")");
  }

  bool has_style = out.count > 0;
  ncc_free(content);
  ncc_free(sl.segs);
  ncc_buffer_free(tb);
  ncc_free(out.items);

  return (ncc_rstr_managed_expr_t){
      .expr = expr ? ncc_buffer_take(expr) : nullptr,
      .has_style = has_style,
  };
}

// =========================================================================
// Main transform
// =========================================================================

static ncc_parse_tree_t *xform_rstr(ncc_xform_ctx_t *ctx,
                                    ncc_parse_tree_t *node) {
  // Match: postfix_expression that looks like a function call.
  // A call has children: callee ( arglist ) or callee ( )
  size_t nc = ncc_tree_num_children(node);

  if (nc < 3) {
    return nullptr;
  }

  // Check that child[1] is "(".
  ncc_parse_tree_t *paren = ncc_tree_child(node, 1);

  if (!paren || !ncc_xform_leaf_text_eq(paren, "(")) {
    return nullptr;
  }

  // Check callee is __ncc_rstr.
  const char *name = rstr_get_callee_name(node);

  if (!name || strcmp(name, "__ncc_rstr") != 0) {
    return nullptr;
  }

  // Get the argument subtree.
  // CALL: kid[0]=callee, kid[1]="(", kid[2]=arglist or ")", kid[3]=")"
  ncc_parse_tree_t *kid2 = ncc_tree_child(node, 2);

  if (!kid2) {
    fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
    exit(1);
  }

  // kid2 might be the ")" if no arguments.
  if (ncc_xform_leaf_text_eq(kid2, ")")) {
    fprintf(stderr, "ncc: error: __ncc_rstr() requires a string argument\n");
    exit(1);
  }

  ncc_parse_tree_t *arglist = kid2;

  // Extract string content from the argument.
  int content_len;
  char *content = extract_rstr_content(arglist, &content_len);

  if (!content) {
    fprintf(stderr,
            "ncc: error: __ncc_rstr() argument must be a string literal\n");
    exit(1);
  }

  // Parse rich markup.
  rstr_seg_list_t sl = {0};
  ncc_buffer_t *tb = ncc_buffer_empty();

  parse_rich_markup(content, content_len, &sl, tb);

  // WP-011 Phase 5d: precompute the descriptor's `.cached_hash` slot
  // from the post-rich-markup UTF-8 content.  Every r-string emission
  // (not just dict-key paths) now lands the content-XXH3 in the
  // descriptor so the runtime `n00b_hash()` short-circuit (D-066)
  // returns the same value regardless of which call site emitted the
  // r-string.  See `format_rstr_cached_hash_expr` near the top of this
  // file for the algorithm and the `_BitInt(128)` literal shape.
  char *cached_hash_expr =
      format_rstr_cached_hash_expr(tb->data, tb->byte_len);

  // Build style records.
  out_style_list_t out = {0};

  build_style_records(&sl, &out);

  // Generate the replacement tree via template engine.
  int uid = ctx->unique_id++;
  ncc_template_registry_t *tmpl_reg = get_template_reg(ctx);

  int64_t cp_count = count_utf8_codepoints(tb->data, (int64_t)tb->byte_len);

  // Build common argument strings.
  char var_name[64];
  snprintf(var_name, sizeof(var_name), "_ncc_rs_%d", uid);

  char bytes_str[32];
  snprintf(bytes_str, sizeof(bytes_str), "%zu", tb->byte_len);

  char cp_str[32];
  snprintf(cp_str, sizeof(cp_str), "%lld", (long long)cp_count);

  // Escaped data literal (with quotes).
  ncc_buffer_t *data_buf = ncc_buffer_empty();
  ncc_buffer_putc(data_buf, '"');
  emit_escaped_string(data_buf, tb->data, (int)tb->byte_len);
  ncc_buffer_putc(data_buf, '"');
  char *data_str = ncc_buffer_take(data_buf);

  // Extra args for n00b build (typehash + wrapper var name).
  // These are ignored by the default template (fewer slots), but
  // consumed by the n00b template override which adds more slots.
  // Hash the POINTER form: the descriptor's tinfo must match the runtime
  // type registry (keyed by typehash(T*)) so a static r-string resolves to
  // the same n00b_string_t type_info as a heap string. get_rstr_string_type
  // is configured as a value type (n00b's array-literal type matcher relies
  // on that), so use the '*'-normalizing helper here.
  char *typehash_str = ncc_static_object_ptr_typehash_expr(get_rstr_string_type(ctx));

  char wrapper_var[64];
  snprintf(wrapper_var, sizeof(wrapper_var), "_ncc_rsh_%d", uid);

  ncc_static_object_names_t names;
  ncc_static_object_names_for_rstr(&names, uid);

  ncc_static_object_slots_t stobj;
  ncc_static_object_slots_init(&stobj, ctx, &names, typehash_str, "2", "1",
                               "nullptr", "nullptr",
                               "N00B_STATIC_IDENTITY_NCC_RSTR", "ncc-rstr",
                               node, bytes_str);

  ncc_result_t(ncc_parse_tree_ptr_t) r;

  if (out.count > 0) {
    // Styled template.
    // Slot layout: $0=style_decls $1=var $2=bytes $3=data
    //              $4=codepoints $5=styling $6=typehash $7=wrapper_var
    //              $8=descriptor $9=entry $10=object_id $11=flags
    //              $12=scan_kind $13=scan_cb $14=scan_user $15=entry_attr
    //              $16=identity_decl $17=identity_expr
    //              $18=cached_hash (WP-011 Phase 5d — XXH3_128bits over
    //                  the post-rich-markup UTF-8 content for every
    //                  emission, including standalone r-string sites).
    char *style_decls = emit_style_declarations(ctx, &out, uid);

    char styling_str[64];
    snprintf(styling_str, sizeof(styling_str), "&_ncc_rs_%d_si", uid);

    const char *all_args[] = {
        style_decls, var_name,    bytes_str,    data_str,
        cp_str,      styling_str, typehash_str, wrapper_var,
        stobj.desc_name,    stobj.entry_name,  stobj.object_id, stobj.flags,
        stobj.scan_kind,    stobj.scan_cb,     stobj.scan_user, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
        cached_hash_expr,
    };
    int nslots = ncc_template_slot_count(tmpl_reg, "rstr_styled");
    require_rstr_template_slots("rstr_styled", nslots, 19);
    r = ncc_template_instantiate(tmpl_reg, "rstr_styled", all_args, nslots);
    ncc_free(style_decls);
  } else {
    // Plain template.
    // Slot layout: $0=var $1=bytes $2=data $3=codepoints
    //              $4=typehash $5=wrapper_var
    //              $6=descriptor $7=entry $8=object_id $9=flags
    //              $10=scan_kind $11=scan_cb $12=scan_user $13=entry_attr
    //              $14=identity_decl $15=identity_expr
    //              $16=cached_hash (WP-011 Phase 5d — XXH3_128bits over
    //                  the post-rich-markup UTF-8 content for every
    //                  emission, including standalone r-string sites).
    const char *all_args[] = {
        var_name, bytes_str, data_str, cp_str, typehash_str, wrapper_var,
        stobj.desc_name, stobj.entry_name, stobj.object_id, stobj.flags,
        stobj.scan_kind, stobj.scan_cb,    stobj.scan_user, stobj.entry_attr,
        stobj.identity_decl, stobj.identity_expr,
        cached_hash_expr,
    };
    int nslots = ncc_template_slot_count(tmpl_reg, "rstr_plain");
    require_rstr_template_slots("rstr_plain", nslots, 17);
    r = ncc_template_instantiate(tmpl_reg, "rstr_plain", all_args, nslots);
  }

  if (ncc_result_is_err(r)) {
    fprintf(stderr,
            "ncc: error: failed to instantiate rstr template "
            "(err=%d)\n",
            r.err);
    exit(1);
  }

  ncc_parse_tree_t *replacement = ncc_result_get(r);

  // Cleanup.
  ncc_static_object_slots_cleanup(&stobj);
  ncc_free(content);
  ncc_free(data_str);
  ncc_free(typehash_str);
  ncc_free(cached_hash_expr);
  ncc_free(sl.segs);
  ncc_buffer_free(tb);
  ncc_free(out.items);

  return replacement;
}

// =========================================================================
// Registration
// =========================================================================

void ncc_register_rstr_xform(ncc_xform_registry_t *reg) {
  ncc_xform_register(reg, "postfix_expression", xform_rstr, "rstr");
}
