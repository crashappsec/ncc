// xform_template.c — Parameterized code template engine for ncc transforms.
//
// Pre-lexes fixed portions of C templates at registration time.
// At instantiation, only the dynamic $N argument strings are lexed,
// spliced into the pre-lexed token runs, and parsed as a unit.

#include "xform/xform_template.h"
#include "internal/parse/grammar_internal.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/string.h"
#include "parse/pwz.h"
#include "scanner/token_stream.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Internal: lex a string into a token pointer array
// ============================================================================

typedef struct {
  ncc_token_info_t **tokens;
  int32_t count;
  int32_t cap;
} token_array_t;

static void token_array_push(token_array_t *ta, ncc_token_info_t *tok) {
  if (ta->count >= ta->cap) {
    int32_t new_cap = ta->cap ? ta->cap * 2 : 32;
    ncc_token_info_t **new_arr =
        ncc_realloc(ta->tokens, (size_t)new_cap * sizeof(ncc_token_info_t *));
    ta->tokens = new_arr;
    ta->cap = new_cap;
  }
  ta->tokens[ta->count++] = tok;
}

// Deep-copy a token so it survives scanner/stream cleanup.
// Follows the pattern from ncc_xform_clone (transform.c).
static ncc_token_info_t *deep_copy_token(ncc_token_info_t *orig) {
  ncc_token_info_t *copy = ncc_alloc(ncc_token_info_t);
  *copy = *orig;

  if (ncc_option_is_set(orig->value)) {
    ncc_string_t val = ncc_option_get(orig->value);
    if (val.data && val.u8_bytes > 0) {
      ncc_string_t val_copy =
          ncc_string_from_raw(val.data, (int64_t)val.u8_bytes);
      copy->value = ncc_option_set(ncc_string_t, val_copy);
    }
  }

  // Trivia not needed for template tokens — clear to avoid dangling refs.
  copy->leading_trivia = nullptr;
  copy->trailing_trivia = nullptr;

  return copy;
}

// Lex source into a freshly allocated token array.
// Returns true on success, false if lexing produces zero tokens.
static bool lex_to_token_array(const char *source, ncc_scan_cb_t cb,
                               ncc_grammar_t *grammar, token_array_t *out) {
  out->tokens = nullptr;
  out->count = 0;
  out->cap = 0;

  if (!source || source[0] == '\0') {
    return true; // empty source → empty array, that's fine
  }

  size_t src_len = strlen(source);
  ncc_buffer_t *buf = ncc_buffer_from_bytes(source, (int64_t)src_len);
  ncc_scanner_t *scanner = ncc_scanner_new(
      buf, cb, grammar, ncc_option_none(ncc_string_t), nullptr, nullptr);
  ncc_token_stream_t *ts = ncc_token_stream_new(scanner);

  ncc_token_info_t *tok;
  while ((tok = ncc_stream_next(ts)) != nullptr) {
    token_array_push(out, deep_copy_token(tok));
  }

  ncc_token_stream_free(ts);
  ncc_scanner_free(scanner);

  return true;
}

static void token_array_free(token_array_t *ta) {
  // Tokens are ncc_alloc'd — not individually freed.
  ncc_free(ta->tokens);
  ta->tokens = nullptr;
  ta->count = 0;
  ta->cap = 0;
}

// ============================================================================
// Registry init / free
// ============================================================================

void ncc_template_registry_init(ncc_template_registry_t *reg,
                                ncc_grammar_t *grammar,
                                ncc_scan_cb_t tokenize) {
  assert(reg && grammar && tokenize);
  ncc_dict_init(&reg->templates, ncc_hash_cstring, ncc_dict_cstr_eq);
  reg->grammar = grammar;
  reg->tokenize = tokenize;
}

void ncc_template_registry_free(ncc_template_registry_t *reg) {
  if (!reg) {
    return;
  }

  // Walk the dict buckets and free compiled templates.
  ncc_dict_t *d = &reg->templates;

  for (size_t i = 0; i < d->capacity; i++) {
    if (d->buckets[i].state != _NCC_BUCKET_OCCUPIED) {
      continue;
    }

    ncc_compiled_template_t *ct = d->buckets[i].value;

    for (int32_t s = 0; s < ct->num_segments; s++) {
      ncc_free(ct->segments[s].tokens);
    }
    ncc_free(ct->segments);
    ncc_free(ct->name.data);
    ncc_free(ct->start_symbol.data);
    ncc_free(ct);
  }

  ncc_dict_free(&reg->templates);
}

// ============================================================================
// Template compilation (register)
// ============================================================================

bool ncc_template_register(ncc_template_registry_t *reg, const char *name,
                           const char *start_symbol,
                           const char *template_text) {
  assert(reg && name && start_symbol && template_text);

  // Scan template_text for $N slots.
  // Collect segments: each is (fixed text before slot, slot_index).

  // Temporary growable segment list.
  int32_t seg_cap = 8;
  int32_t seg_cnt = 0;
  ncc_template_segment_t *segs =
      ncc_alloc_array(ncc_template_segment_t, (size_t)seg_cap);

  int32_t max_slot = -1;
  const char *p = template_text;
  const char *run_st = p;

  while (*p) {
    if (*p == '$' && isdigit((unsigned char)p[1])) {
      // Flush the fixed text before this slot.
      size_t run_len = (size_t)(p - run_st);
      char *fixed = ncc_alloc_size(1, run_len + 1);
      memcpy(fixed, run_st, run_len);
      fixed[run_len] = '\0';

      // Parse slot number.
      p++; // skip '$'
      int32_t slot = 0;
      while (isdigit((unsigned char)*p)) {
        slot = slot * 10 + (*p - '0');
        p++;
      }
      if (slot > max_slot) {
        max_slot = slot;
      }

      // Lex the fixed text.
      token_array_t ta;
      if (!lex_to_token_array(fixed, reg->tokenize, reg->grammar, &ta)) {
        ncc_free(fixed);
        ncc_free(segs);
        return false;
      }
      ncc_free(fixed);

      // Grow segment array if needed.
      if (seg_cnt >= seg_cap) {
        seg_cap *= 2;
        segs =
            ncc_realloc(segs, (size_t)seg_cap * sizeof(ncc_template_segment_t));
      }

      segs[seg_cnt++] = (ncc_template_segment_t){
          .tokens = ta.tokens,
          .count = ta.count,
          .slot_index = slot,
      };

      run_st = p;
    } else {
      p++;
    }
  }

  // Trailing text (no slot after it).
  if (p > run_st) {
    size_t run_len = (size_t)(p - run_st);
    char *fixed = ncc_alloc_size(1, run_len + 1);
    memcpy(fixed, run_st, run_len);
    fixed[run_len] = '\0';

    token_array_t ta;
    if (!lex_to_token_array(fixed, reg->tokenize, reg->grammar, &ta)) {
      ncc_free(fixed);
      // Free already-collected segments.
      for (int32_t i = 0; i < seg_cnt; i++) {
        ncc_free(segs[i].tokens);
      }
      ncc_free(segs);
      return false;
    }
    ncc_free(fixed);

    if (seg_cnt >= seg_cap) {
      seg_cap *= 2;
      segs =
          ncc_realloc(segs, (size_t)seg_cap * sizeof(ncc_template_segment_t));
    }

    segs[seg_cnt++] = (ncc_template_segment_t){
        .tokens = ta.tokens,
        .count = ta.count,
        .slot_index = -1,
    };
  }

  // Build compiled template.
  ncc_compiled_template_t *ct = ncc_alloc(ncc_compiled_template_t);
  ct->name = ncc_string_from_cstr(name);
  ct->start_symbol = ncc_string_from_cstr(start_symbol);
  ct->segments = segs;
  ct->num_segments = seg_cnt;
  ct->num_slots = max_slot + 1;

  ncc_dict_put(&reg->templates, ct->name.data, ct);

  return true;
}

// ============================================================================
// Template instantiation
// ============================================================================

ncc_result_t(ncc_parse_tree_ptr_t)
    ncc_template_instantiate(ncc_template_registry_t *reg, const char *name,
                             const char **args, int nargs) {
  assert(reg && name);

  // 1. Look up compiled template.
  bool found = false;
  ncc_compiled_template_t *ct =
      ncc_dict_get(&reg->templates, (void *)name, &found);

  if (!found) {
    return ncc_result_err(ncc_parse_tree_ptr_t, NCC_TMPL_ERR_NOT_FOUND);
  }

  // 2. Check argument count.
  if (nargs != ct->num_slots) {
    return ncc_result_err(ncc_parse_tree_ptr_t, NCC_TMPL_ERR_ARG_COUNT);
  }

  // 3. Lex each argument string.
  token_array_t *arg_tokens = nullptr;

  if (nargs > 0) {
    arg_tokens = ncc_alloc_array(token_array_t, (size_t)nargs);

    for (int i = 0; i < nargs; i++) {
      if (!lex_to_token_array(args[i], reg->tokenize, reg->grammar,
                              &arg_tokens[i])) {
        // Cleanup already-lexed args.
        for (int j = 0; j < i; j++) {
          token_array_free(&arg_tokens[j]);
        }
        ncc_free(arg_tokens);
        return ncc_result_err(ncc_parse_tree_ptr_t, NCC_TMPL_ERR_ARG_LEX);
      }
    }
  }

  // 4. Concatenate all tokens: fixed segments interleaved with arg tokens.
  token_array_t all = {0};

  for (int32_t s = 0; s < ct->num_segments; s++) {
    ncc_template_segment_t *seg = &ct->segments[s];

    // Append fixed tokens (deep-copy so each instantiation is independent).
    for (int32_t t = 0; t < seg->count; t++) {
      token_array_push(&all, deep_copy_token(seg->tokens[t]));
    }

    // Append arg tokens for this slot.
    if (seg->slot_index >= 0 && seg->slot_index < nargs) {
      token_array_t *at = &arg_tokens[seg->slot_index];
      for (int32_t t = 0; t < at->count; t++) {
        token_array_push(&all, deep_copy_token(at->tokens[t]));
      }
    }
  }

  // 5. Re-index all tokens sequentially (critical for PWZ memoization).
  for (int32_t i = 0; i < all.count; i++) {
    all.tokens[i]->index = i;
  }

  // 6. Build token stream from assembled array.
  ncc_token_stream_t *ts = ncc_token_stream_from_array(all.tokens, all.count);

  // 7. Temporarily override grammar default_start.
  ncc_string_t nt_str = ct->start_symbol;
  ncc_nonterm_t *nt = ncc_nonterm(reg->grammar, nt_str);
  int32_t saved_start = reg->grammar->default_start;

  if (nt) {
    reg->grammar->default_start = (int32_t)ncc_nonterm_id(nt);
  }

  // 8. Parse.
  ncc_pwz_parser_t *parser = ncc_pwz_new(reg->grammar);
  reg->grammar->default_start = saved_start;

  bool ok = ncc_pwz_parse(parser, ts);

  ncc_result_t(ncc_parse_tree_ptr_t) ret;

  if (ok) {
    ncc_parse_tree_t *tree = ncc_pwz_get_tree(parser);
    ncc_parse_tree_t *cloned = ncc_xform_clone(tree);
    ncc_xform_set_parent_pointers(cloned);
    ret = ncc_result_ok(ncc_parse_tree_ptr_t, cloned);
  } else {
    ret = ncc_result_err(ncc_parse_tree_ptr_t, NCC_TMPL_ERR_PARSE_FAILED);
  }

  // 9. Cleanup.
  ncc_pwz_free(parser);
  ncc_token_stream_free(ts);
  ncc_free(all.tokens);

  if (arg_tokens) {
    for (int i = 0; i < nargs; i++) {
      token_array_free(&arg_tokens[i]);
    }
    ncc_free(arg_tokens);
  }

  return ret;
}

int ncc_template_slot_count(ncc_template_registry_t *reg, const char *name) {
  assert(reg && name);

  bool found = false;
  ncc_compiled_template_t *ct =
      ncc_dict_get(&reg->templates, (void *)name, &found);

  if (!found) {
    return -1;
  }

  return ct->num_slots;
}
