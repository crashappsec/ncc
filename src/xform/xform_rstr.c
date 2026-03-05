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
#include "xform/xform_helpers.h"
#include "xform/xform_template.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =========================================================================
// Layout-compatible access to ncc_xform_data_t.template_reg
// =========================================================================
// Must match the struct layout in ncc.c (same pattern as
// xform_generic_struct.c).

#define _NCC_META_TABLE_SIZE 256

typedef struct {
  char *key;
  void *value;
} _rstr_meta_entry_t;

typedef struct {
  _rstr_meta_entry_t entries[_NCC_META_TABLE_SIZE];
} _rstr_meta_table_t;

typedef struct {
  const char *compiler;
  const char *constexpr_headers;
  _rstr_meta_table_t func_meta;
  ncc_dict_t option_meta;
  ncc_dict_t option_decls;
  ncc_dict_t generic_struct_decls;
  ncc_template_registry_t *template_reg;
  const char *vargs_type;
  const char *once_prefix;
  const char *rstr_string_type;
} _rstr_xform_data_t;

static ncc_template_registry_t *get_template_reg(ncc_xform_ctx_t *ctx) {
  _rstr_xform_data_t *d = ctx->user_data;
  return d->template_reg;
}

static const char *get_rstr_string_type(ncc_xform_ctx_t *ctx) {
  _rstr_xform_data_t *d = ctx->user_data;
  return d->rstr_string_type;
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

static void emit_escaped_string(FILE *f, const char *data, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)data[i];

    switch (c) {
    case '\\':
      fputs("\\\\", f);
      break;
    case '"':
      fputs("\\\"", f);
      break;
    case '\n':
      fputs("\\n", f);
      break;
    case '\r':
      fputs("\\r", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    case '\0':
      fputs("\\0", f);
      break;
    default:
      if (c < 0x20 || c == 0x7f) {
        fprintf(f, "\\x%02x", c);
      } else {
        fputc(c, f);
      }
      break;
    }
  }
}

static void emit_style_var(FILE *f, out_style_t *os, int idx, int uid) {
  if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
    // Deferred: info = nullptr, tag = "name". No style variable needed.
  } else if (os->kind == PSTYLE_CASE) {
    fprintf(f,
            "static ncc_text_style_t _ncc_rs_%d_ts_%d="
            "{.text_case=%d};",
            uid, idx, os->case_val);
  } else {
    fprintf(f,
            "static ncc_text_style_t _ncc_rs_%d_ts_%d="
            "{.%s=2};",
            uid, idx, os->field_name);
  }
}

// Emit style variable declarations and style info struct as a string.
// This becomes the $0 argument to the rstr_styled template.
// Returns an allocated string (caller frees).
static char *emit_style_declarations(out_style_list_t *out, int uid) {
  char *result = nullptr;
  size_t size;
  FILE *f = open_memstream(&result, &size);

  for (int i = 0; i < out->count; i++) {
    emit_style_var(f, &out->items[i], i, uid);
  }

  fprintf(f,
          "static struct{"
          "int64_t num_styles;"
          "ncc_text_style_t*base_style;"
          "ncc_style_record_t styles[%d];"
          "}_ncc_rs_%d_si={",
          out->count, uid);
  fprintf(f, ".num_styles=%d,.base_style=((ncc_text_style_t*)0),", out->count);
  fprintf(f, ".styles={");

  for (int i = 0; i < out->count; i++) {
    if (i > 0) {
      fputc(',', f);
    }

    out_style_t *os = &out->items[i];

    fprintf(f, "{");

    if (os->kind == PSTYLE_NAMED || os->kind == PSTYLE_ROLE) {
      fprintf(f, ".info=((ncc_text_style_t*)0),.tag=\"");
      fputs(os->field_name, f);
      fprintf(f, "\"");
    } else {
      fprintf(f, ".info=&_ncc_rs_%d_ts_%d,.tag=((void*)0)", uid, i);
    }

    fprintf(f, ",.start=%d", os->start);

    if (os->end >= 0) {
      fprintf(f, ",.end={.has_value=1,.value=(size_t)%d}", os->end);
    } else {
      fprintf(f, ",.end={.has_value=0}");
    }

    fprintf(f, "}");
  }

  fprintf(f, "}};");

  fclose(f);
  return result;
}

// =========================================================================
// String literal extraction
// =========================================================================

static char *extract_rstr_content(ncc_parse_tree_t *arglist, int *out_len) {
  char *result = nullptr;
  size_t size;
  FILE *f = open_memstream(&result, &size);

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
              fputc('\n', f);
              p++;
              break;
            case 'r':
              fputc('\r', f);
              p++;
              break;
            case 't':
              fputc('\t', f);
              p++;
              break;
            case '0':
              fputc('\0', f);
              p++;
              break;
            case '\\':
              fputc('\\', f);
              p++;
              break;
            case '"':
              fputc('"', f);
              p++;
              break;
            case 'a':
              fputc('\a', f);
              p++;
              break;
            case 'b':
              fputc('\b', f);
              p++;
              break;
            case 'f':
              fputc('\f', f);
              p++;
              break;
            case 'v':
              fputc('\v', f);
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

              fputc((char)val, f);
              break;
            }
            default:
              fputc('\\', f);
              fputc(*p, f);
              p++;
              break;
            }
          } else {
            fputc(*p, f);
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
  fclose(f);
  *out_len = (int)size;
  return result;
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
  char *data_str = nullptr;
  size_t data_size;
  FILE *df = open_memstream(&data_str, &data_size);
  fputc('"', df);
  emit_escaped_string(df, tb->data, (int)tb->byte_len);
  fputc('"', df);
  fclose(df);

  // Extra args for n00b build (typehash + wrapper var name).
  // These are ignored by the default template (fewer slots), but
  // consumed by the n00b template override which adds more slots.
  uint64_t str_typehash = ncc_type_hash_u64(get_rstr_string_type(ctx));
  char typehash_str[32];
  snprintf(typehash_str, sizeof(typehash_str), "%" PRIu64 "ULL", str_typehash);

  char wrapper_var[64];
  snprintf(wrapper_var, sizeof(wrapper_var), "_ncc_rsh_%d", uid);

  ncc_result_t(ncc_parse_tree_ptr_t) r;

  if (out.count > 0) {
    // Styled template.
    // Slot layout: $0=style_decls $1=var $2=bytes $3=data
    //              $4=codepoints $5=styling $6=typehash $7=wrapper_var
    char *style_decls = emit_style_declarations(&out, uid);

    char styling_str[64];
    snprintf(styling_str, sizeof(styling_str), "&_ncc_rs_%d_si", uid);

    const char *all_args[] = {
        style_decls, var_name,    bytes_str,    data_str,
        cp_str,      styling_str, typehash_str, wrapper_var,
    };
    int nslots = ncc_template_slot_count(tmpl_reg, "rstr_styled");
    r = ncc_template_instantiate(tmpl_reg, "rstr_styled", all_args, nslots);
    ncc_free(style_decls);
  } else {
    // Plain template.
    // Slot layout: $0=var $1=bytes $2=data $3=codepoints
    //              $4=typehash $5=wrapper_var
    const char *all_args[] = {
        var_name, bytes_str, data_str, cp_str, typehash_str, wrapper_var,
    };
    int nslots = ncc_template_slot_count(tmpl_reg, "rstr_plain");
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
  ncc_free(content);
  ncc_free(data_str);
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
