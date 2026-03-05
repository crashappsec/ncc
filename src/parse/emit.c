// emit.c — Wadler/Lindig pretty-printer for parse tree emission.
//
// Two-phase approach:
// Phase 1: Tree walk -> document command stream
// Phase 2: Layout resolution -> formatted output
//
// Formatting rules come from the style table passed via emit opts.

#include "parse/emit.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/string.h"
#include "internal/parse/grammar_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Document stream
// ============================================================================

typedef struct {
    ncc_doc_kind_t kind;
    const char     *text;
    int32_t         text_len;
} doc_cmd_t;

typedef struct {
    doc_cmd_t *cmds;
    int32_t    count;
    int32_t    cap;
} doc_stream_t;

static void
doc_init(doc_stream_t *ds)
{
    ds->cmds  = nullptr;
    ds->count = 0;
    ds->cap   = 0;
}

static void
doc_free(doc_stream_t *ds)
{
    ncc_free(ds->cmds);
    ds->cmds  = nullptr;
    ds->count = 0;
    ds->cap   = 0;
}

static void
doc_emit(doc_stream_t *ds, ncc_doc_kind_t kind, const char *text, int32_t len)
{
    if (ds->count >= ds->cap) {
        ds->cap  = ds->cap ? ds->cap * 2 : 64;
        ds->cmds = ncc_realloc(ds->cmds, (size_t)ds->cap * sizeof(doc_cmd_t));
    }

    ds->cmds[ds->count++] = (doc_cmd_t){
        .kind     = kind,
        .text     = text,
        .text_len = len,
    };
}

static inline void
doc_text(doc_stream_t *ds, const char *text, int32_t len)
{
    doc_emit(ds, NCC_DOC_TEXT, text, len);
}

static inline void
doc_space(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_SPACE, nullptr, 0);
}

static inline void
doc_softline(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_SOFTLINE, nullptr, 0);
}

static inline void
doc_hardline(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_HARDLINE, nullptr, 0);
}

static inline void
doc_blankline(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_BLANKLINE, nullptr, 0);
}

static inline void
doc_indent(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_INDENT, nullptr, 0);
}

static inline void
doc_dedent(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_DEDENT, nullptr, 0);
}

static inline void
doc_group_begin(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_GROUP_BEGIN, nullptr, 0);
}

static inline void
doc_group_end(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_GROUP_END, nullptr, 0);
}

static inline void
doc_align_begin(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_ALIGN_BEGIN, nullptr, 0);
}

static inline void
doc_align_end(doc_stream_t *ds)
{
    doc_emit(ds, NCC_DOC_ALIGN_END, nullptr, 0);
}

// ============================================================================
// Style table lookup
// ============================================================================

static const ncc_pprint_rule_t *
style_lookup(ncc_pprint_style_t style, const char *nt_name)
{
    if (!style || !nt_name) {
        return nullptr;
    }

    for (ncc_pprint_rule_t *r = style; r->nt_name; r++) {
        if (strcmp(r->nt_name, nt_name) == 0) {
            return r;
        }
    }

    return nullptr;
}

// ============================================================================
// Default spacing heuristics
// ============================================================================

typedef enum {
    TOK_CAT_OPEN_BRACKET,
    TOK_CAT_CLOSE_BRACKET,
    TOK_CAT_COMMA,
    TOK_CAT_SEMICOLON,
    TOK_CAT_DOT,
    TOK_CAT_OTHER,
} tok_category_t;

static tok_category_t
categorize_token(const char *text)
{
    if (!text || !*text) {
        return TOK_CAT_OTHER;
    }

    if ((text[0] == '(' || text[0] == '[' || text[0] == '{') && !text[1]) {
        return TOK_CAT_OPEN_BRACKET;
    }

    if ((text[0] == ')' || text[0] == ']' || text[0] == '}') && !text[1]) {
        return TOK_CAT_CLOSE_BRACKET;
    }

    if (text[0] == ',' && !text[1])
        return TOK_CAT_COMMA;
    if (text[0] == ';' && !text[1])
        return TOK_CAT_SEMICOLON;
    if (text[0] == '.' && !text[1])
        return TOK_CAT_DOT;
    if (text[0] == '-' && text[1] == '>' && !text[2])
        return TOK_CAT_DOT;

    return TOK_CAT_OTHER;
}

static bool
heuristic_space(tok_category_t prev, tok_category_t next)
{
    if (prev == TOK_CAT_OPEN_BRACKET)
        return false;
    if (next == TOK_CAT_CLOSE_BRACKET)
        return false;
    if (next == TOK_CAT_COMMA)
        return false;
    if (next == TOK_CAT_SEMICOLON)
        return false;
    if (prev == TOK_CAT_DOT)
        return false;
    if (next == TOK_CAT_DOT)
        return false;
    return true;
}

// ============================================================================
// Emit context
// ============================================================================

typedef struct {
    doc_stream_t       *ds;
    ncc_grammar_t     *grammar;
    ncc_pprint_style_t style;
    tok_category_t      last_tok_cat;
    bool                need_space;
    bool                first_token;
} emit_ctx_t;

static bool
in_index_array(const int32_t *arr, int32_t ix)
{
    if (!arr) {
        return false;
    }

    for (int i = 0; arr[i] >= 0; i++) {
        if (arr[i] == ix) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Trivia emission
// ============================================================================

static void
emit_trivia(emit_ctx_t *ctx, ncc_trivia_t *trivia)
{
    for (ncc_trivia_t *t = trivia; t; t = t->next) {
        if (t->text.data && t->text.u8_bytes > 0) {
            doc_text(ctx->ds, t->text.data, (int32_t)t->text.u8_bytes);
        }
    }
}

// ============================================================================
// Tree walk: emit document commands
// ============================================================================

static void emit_tree(emit_ctx_t *ctx, ncc_parse_tree_t *node);

static void
emit_token(emit_ctx_t *ctx, ncc_token_info_t *tok)
{
    if (!tok || !ncc_option_is_set(tok->value)) {
        return;
    }

    ncc_string_t val = ncc_option_get(tok->value);

    if (!val.data || val.u8_bytes == 0) {
        return;
    }

    if (tok->leading_trivia) {
        emit_trivia(ctx, tok->leading_trivia);
        ctx->need_space = false;
    }
    else {
        tok_category_t cat = categorize_token(val.data);

        if (!ctx->first_token && ctx->need_space
            && heuristic_space(ctx->last_tok_cat, cat)) {
            doc_space(ctx->ds);
        }

        ctx->last_tok_cat = cat;
    }

    doc_text(ctx->ds, val.data, (int32_t)val.u8_bytes);
    ctx->first_token = false;
    ctx->need_space  = true;

    ctx->last_tok_cat = categorize_token(val.data);

    if (tok->trailing_trivia) {
        emit_trivia(ctx, tok->trailing_trivia);
    }
}

static void
emit_nt(emit_ctx_t *ctx, ncc_parse_tree_t *node, ncc_nt_node_t *pn)
{
    ncc_nonterm_t *nt = ncc_get_nonterm(ctx->grammar, pn->id);

    // Get NT name for style lookup.
    const char *name = nullptr;

    if (nt && nt->name.data) {
        name = nt->name.data;
    }

    // Tier 1: style table lookup.
    const ncc_pprint_rule_t *rule = style_lookup(ctx->style, name);

    bool           do_indent = false;
    bool           do_group  = false;
    bool           do_concat = false;
    bool           do_blank  = false;
    const int32_t *hardlines = nullptr;
    const int32_t *softlines = nullptr;
    const int32_t *nospaces  = nullptr;
    const int32_t *aligns    = nullptr;

    if (rule) {
        do_indent = rule->indent;
        do_group  = rule->group;
        do_concat = rule->concat;
        do_blank  = rule->blankline_after;
        hardlines = rule->hardline_before;
        softlines = rule->softline_before;
        nospaces  = rule->nospace_before;
        aligns    = rule->align_to;
    }

    if (do_group) {
        doc_group_begin(ctx->ds);
    }

    size_t nc = ncc_tree_num_children(node);

    // Find the last hardline child index so we can dedent before it
    // (closing delimiter at parent indent level).
    int32_t last_hardline_child = -1;

    if (do_indent && hardlines) {
        for (int k = 0; hardlines[k] >= 0; k++) {
            if (hardlines[k] > last_hardline_child) {
                last_hardline_child = hardlines[k];
            }
        }
    }

    if (do_indent) {
        doc_indent(ctx->ds);
    }

    bool saved_need_space = ctx->need_space;
    bool did_dedent       = false;

    if (do_concat) {
        ctx->need_space = false;
    }

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *child = ncc_tree_child(node, i);

        if (!child) {
            continue;
        }

        // Skip missing children.
        if (!ncc_tree_is_leaf(child)) {
            ncc_nt_node_t *cpn = &ncc_tree_node_value(child);

            if (cpn->missing) {
                continue;
            }
        }

        int32_t ci = (int32_t)i;

        // Emit dedent before the last hardline child so the closing
        // delimiter sits at the parent indent level.
        if (do_indent && !did_dedent && ci == last_hardline_child) {
            doc_dedent(ctx->ds);
            did_dedent = true;
        }

        if (in_index_array(hardlines, ci)) {
            doc_hardline(ctx->ds);
            ctx->need_space = false;
        }
        else if (in_index_array(softlines, ci)) {
            doc_softline(ctx->ds);
            ctx->need_space = false;
        }

        if (in_index_array(nospaces, ci)) {
            ctx->need_space = false;
        }

        bool align = in_index_array(aligns, ci);

        if (align) {
            doc_align_begin(ctx->ds);
        }

        emit_tree(ctx, child);

        if (align) {
            doc_align_end(ctx->ds);
        }

        if (do_concat) {
            ctx->need_space = false;
        }
    }

    if (do_indent && !did_dedent) {
        doc_dedent(ctx->ds);
    }

    if (do_group) {
        doc_group_end(ctx->ds);
    }

    if (do_blank) {
        doc_blankline(ctx->ds);
    }

    if (do_concat) {
        ctx->need_space = saved_need_space;
    }
}

static void
emit_tree(emit_ctx_t *ctx, ncc_parse_tree_t *node)
{
    if (!node) {
        return;
    }

    // Token leaf.
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        emit_token(ctx, tok);
        return;
    }

    ncc_nt_node_t *pn = &ncc_tree_node_value(node);

    if (pn->missing) {
        return;
    }

    // Group nodes: transparent recursion.
    if (pn->group_top || pn->group_item) {
        size_t nc = ncc_tree_num_children(node);

        for (size_t i = 0; i < nc; i++) {
            emit_tree(ctx, ncc_tree_child(node, i));
        }

        return;
    }

    // Non-terminal.
    emit_nt(ctx, node, pn);
}

// ============================================================================
// Layout engine: resolve document stream to output
// ============================================================================

static int32_t
measure_flat_width(doc_cmd_t *cmds, int32_t start, int32_t end)
{
    int32_t width = 0;
    int     depth = 0;

    for (int32_t i = start; i < end; i++) {
        switch (cmds[i].kind) {
        case NCC_DOC_TEXT:
            width += cmds[i].text_len;
            break;
        case NCC_DOC_SPACE:
        case NCC_DOC_SOFTLINE:
            width += 1;
            break;
        case NCC_DOC_HARDLINE:
        case NCC_DOC_BLANKLINE:
            return INT32_MAX;
        case NCC_DOC_GROUP_BEGIN:
            depth++;
            break;
        case NCC_DOC_GROUP_END:
            depth--;
            if (depth < 0)
                return width;
            break;
        default:
            break;
        }
    }

    return width;
}

static int32_t
find_group_end(doc_cmd_t *cmds, int32_t count, int32_t start)
{
    int depth = 1;

    for (int32_t i = start + 1; i < count; i++) {
        if (cmds[i].kind == NCC_DOC_GROUP_BEGIN)
            depth++;
        if (cmds[i].kind == NCC_DOC_GROUP_END) {
            depth--;
            if (depth == 0)
                return i;
        }
    }

    return count;
}

// ============================================================================
// Growable stack for layout engine (Fix 11)
// ============================================================================

typedef struct {
    int32_t *data;
    int      top;
    int      cap;
} int_stack_t;

static void
istack_init(int_stack_t *st, int init_cap)
{
    st->cap  = init_cap > 0 ? init_cap : 32;
    st->top  = 0;
    st->data = ncc_alloc_array(int32_t, (size_t)st->cap);
    st->data[0] = 0;
}

static void
istack_free(int_stack_t *st)
{
    ncc_free(st->data);
    st->data = nullptr;
    st->top  = 0;
    st->cap  = 0;
}

static void
istack_push(int_stack_t *st, int32_t val)
{
    if (st->top + 1 >= st->cap) {
        st->cap *= 2;
        st->data = ncc_realloc(st->data, (size_t)st->cap * sizeof(int32_t));
    }
    st->data[++st->top] = val;
}

static int32_t
istack_pop(int_stack_t *st)
{
    if (st->top > 0) {
        return st->data[st->top--];
    }
    return st->data[0];
}

static int32_t
istack_peek(int_stack_t *st)
{
    return st->data[st->top];
}

typedef struct {
    bool *data;
    int   top;
    int   cap;
} bool_stack_t;

static void
bstack_init(bool_stack_t *st, int init_cap)
{
    st->cap  = init_cap > 0 ? init_cap : 32;
    st->top  = 0;
    st->data = ncc_alloc_array(bool, (size_t)st->cap);
    st->data[0] = true; // root group is always broken
}

static void
bstack_free(bool_stack_t *st)
{
    ncc_free(st->data);
    st->data = nullptr;
    st->top  = 0;
    st->cap  = 0;
}

static void
bstack_push(bool_stack_t *st, bool val)
{
    if (st->top + 1 >= st->cap) {
        st->cap *= 2;
        st->data = ncc_realloc(st->data, (size_t)st->cap * sizeof(bool));
    }
    st->data[++st->top] = val;
}

static void
bstack_pop(bool_stack_t *st)
{
    if (st->top > 0) {
        st->top--;
    }
}

// ============================================================================
// Layout indentation helpers
// ============================================================================

static void
emit_layout_indent(ncc_buffer_t *ob, int32_t level, int32_t indent_size,
                   ncc_indent_style_t style)
{
    int32_t total = level * indent_size;

    for (int32_t i = 0; i < total; i++) {
        ncc_buffer_putc(ob, (style == NCC_PPRINT_TABS) ? '\t' : ' ');
    }
}

static void
emit_spaces(ncc_buffer_t *ob, int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        ncc_buffer_putc(ob, ' ');
    }
}

// Emit newline + indentation, respecting alignment if active.
// Returns the new column position.
static int32_t
emit_newline_indent(ncc_buffer_t *ob, const char *nl,
                    int_stack_t *indent_st, int32_t indent_size,
                    ncc_indent_style_t style,
                    int_stack_t *align_st)
{
    ncc_buffer_puts(ob, nl);

    // If inside an alignment scope, indent to the alignment column.
    if (align_st->top > 0) {
        int32_t align_col = istack_peek(align_st);
        emit_spaces(ob, align_col);
        return align_col;
    }

    int32_t ind = istack_peek(indent_st);
    emit_layout_indent(ob, ind, indent_size, style);
    return ind * indent_size;
}

static ncc_string_t
layout_resolve(doc_stream_t *ds, ncc_pprint_opts_t *opts)
{
    int32_t     line_width  = opts->line_width > 0 ? opts->line_width : 80;
    int32_t     indent_size = opts->indent_size > 0 ? opts->indent_size : 4;
    const char *nl          = opts->newline ? opts->newline : "\n";

    ncc_buffer_t *ob = ncc_buffer_empty();

    int_stack_t  indent_st;
    bool_stack_t group_st;
    int_stack_t  align_st;

    istack_init(&indent_st, 32);
    bstack_init(&group_st, 32);
    istack_init(&align_st, 32);

    int32_t col = 0;

    for (int32_t i = 0; i < ds->count; i++) {
        doc_cmd_t *cmd = &ds->cmds[i];

        switch (cmd->kind) {
        case NCC_DOC_TEXT:
            ncc_buffer_append(ob, cmd->text, (size_t)cmd->text_len);
            col += cmd->text_len;
            break;

        case NCC_DOC_SPACE:
            ncc_buffer_putc(ob, ' ');
            col++;
            break;

        case NCC_DOC_SOFTLINE:
            if (group_st.top > 0 && !group_st.data[group_st.top]) {
                ncc_buffer_putc(ob, ' ');
                col++;
            }
            else {
                col = emit_newline_indent(ob, nl, &indent_st, indent_size,
                                          opts->indent_style, &align_st);
            }
            break;

        case NCC_DOC_HARDLINE:
            col = emit_newline_indent(ob, nl, &indent_st, indent_size,
                                      opts->indent_style, &align_st);
            break;

        case NCC_DOC_BLANKLINE:
            ncc_buffer_puts(ob, nl);
            col = emit_newline_indent(ob, nl, &indent_st, indent_size,
                                      opts->indent_style, &align_st);
            break;

        case NCC_DOC_INDENT:
            istack_push(&indent_st, istack_peek(&indent_st) + 1);
            break;

        case NCC_DOC_DEDENT:
            istack_pop(&indent_st);
            break;

        case NCC_DOC_GROUP_BEGIN: {
            int32_t end    = find_group_end(ds->cmds, ds->count, i);
            int32_t flat_w = measure_flat_width(ds->cmds, i + 1, end);
            bstack_push(&group_st, (col + flat_w > line_width));
            break;
        }

        case NCC_DOC_GROUP_END:
            bstack_pop(&group_st);
            break;

        case NCC_DOC_ALIGN_BEGIN:
            istack_push(&align_st, col);
            break;

        case NCC_DOC_ALIGN_END:
            istack_pop(&align_st);
            break;
        }
    }

    istack_free(&indent_st);
    bstack_free(&group_st);
    istack_free(&align_st);

    ncc_string_t result = ncc_string_from_raw(ob->data, (int64_t)ob->byte_len);
    ncc_free(ob->data);
    ncc_free(ob);
    return result;
}

// ============================================================================
// Public API: ncc_pprint
// ============================================================================

ncc_string_t
ncc_pprint(ncc_grammar_t    *g,
             ncc_parse_tree_t *tree,
             ncc_pprint_opts_t opts)
{
    if (!g || !tree) {
        return ncc_string_empty();
    }

    doc_stream_t ds;
    doc_init(&ds);

    emit_ctx_t ctx = {
        .ds           = &ds,
        .grammar      = g,
        .style        = opts.style,
        .last_tok_cat = TOK_CAT_OTHER,
        .need_space   = false,
        .first_token  = true,
    };

    emit_tree(&ctx, tree);

    ncc_string_t result = layout_resolve(&ds, &opts);
    doc_free(&ds);

    if (opts.out) {
        if (result.data) {
            fputs(result.data, opts.out);
            fflush(opts.out);
            ncc_free(result.data);
        }
        return (ncc_string_t){0};
    }

    return result;
}

// ============================================================================
// BNF emitter: grammar -> BNF text
// ============================================================================

static const char *
bnf_cc_name(ncc_char_class_t cc)
{
    switch (cc) {
    case NCC_CC_ASCII_DIGIT:
        return "__DIGIT";
    case NCC_CC_ASCII_ALPHA:
        return "__ALPHA";
    case NCC_CC_ASCII_UPPER:
        return "__UPPER";
    case NCC_CC_ASCII_LOWER:
        return "__LOWER";
    case NCC_CC_HEX_DIGIT:
        return "__HEX";
    case NCC_CC_NONZERO_DIGIT:
        return "__NONZERO_DIGIT";
    case NCC_CC_WHITESPACE:
        return "__WHITESPACE";
    case NCC_CC_ID_START:
        return "__ID_START";
    case NCC_CC_ID_CONTINUE:
        return "__ID_CONTINUE";
    case NCC_CC_PRINTABLE:
        return "__PRINTABLE";
    case NCC_CC_UNICODE_DIGIT:
        return "__UNICODE_DIGIT";
    case NCC_CC_JSON_STRING_CHAR:
        return "__JSON_STR";
    case NCC_CC_REGEX_BODY_CHAR:
        return "__REGEX_STR";
    case NCC_CC_NON_WS_PRINTABLE:
        return "__NON_WS_PRINTABLE";
    case NCC_CC_NON_NL_WS:
        return "__NON_NL_WS";
    case NCC_CC_NON_NL_PRINTABLE:
        return "__NON_NL_PRINTABLE";
    }
    return "__UNKNOWN";
}

static void
emit_match_item(ncc_buffer_t *ob, ncc_grammar_t *g, ncc_match_t *m)
{
    switch (m->kind) {
    case NCC_MATCH_EMPTY:
        ncc_buffer_puts(ob, "\"\"");
        break;

    case NCC_MATCH_NT: {
        ncc_nonterm_t *nt = ncc_get_nonterm(g, m->nt_id);
        ncc_buffer_putc(ob, '<');
        if (nt && nt->name.data) {
            ncc_buffer_append(ob, nt->name.data, nt->name.u8_bytes);
        }
        else {
            ncc_buffer_putc(ob, '?');
        }
        ncc_buffer_putc(ob, '>');
        break;
    }

    case NCC_MATCH_TERMINAL: {
        int64_t          tid  = m->terminal_id;
        ncc_terminal_t *term = ncc_get_terminal(g, tid);

        if (term && term->value.data) {
            const char *val       = term->value.data;
            bool        has_alpha = false;

            for (const char *p = val; *p; p++) {
                if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                    || *p == '_') {
                    has_alpha = true;
                    break;
                }
            }

            (void)has_alpha;
            ncc_buffer_puts(ob, "%\"");
            ncc_buffer_puts(ob, val);
            ncc_buffer_putc(ob, '"');
        }
        else if (tid > 0 && tid < 128) {
            ncc_buffer_putc(ob, '"');
            char c = (char)tid;
            ncc_buffer_append(ob, &c, 1);
            ncc_buffer_putc(ob, '"');
        }
        else {
            ncc_buffer_puts(ob, "\"?\"");
        }
        break;
    }

    case NCC_MATCH_ANY:
        ncc_buffer_puts(ob, "__ANY");
        break;

    case NCC_MATCH_CLASS:
        ncc_buffer_puts(ob, bnf_cc_name(m->char_class));
        break;

    case NCC_MATCH_SET:
        ncc_buffer_puts(ob, "(set)");
        break;

    case NCC_MATCH_GROUP: {
        ncc_rule_group_t *grp = (ncc_rule_group_t *)m->group;

        if (grp) {
            ncc_nonterm_t *cnt_nt = ncc_get_nonterm(g, grp->contents_id);

            if (cnt_nt && cnt_nt->name.data) {
                ncc_buffer_putc(ob, '<');
                ncc_buffer_append(ob, cnt_nt->name.data, cnt_nt->name.u8_bytes);
                ncc_buffer_putc(ob, '>');
            }

            if (grp->min == 0 && grp->max == 1)
                ncc_buffer_putc(ob, '?');
            else if (grp->min == 0 && grp->max == 0)
                ncc_buffer_putc(ob, '*');
            else if (grp->min == 1 && grp->max == 0)
                ncc_buffer_putc(ob, '+');
        }
        break;
    }
    }
}

ncc_string_t
ncc_grammar_emit_bnf(ncc_grammar_t    *g,
                        ncc_pprint_opts_t opts)
{
    if (!g) {
        return ncc_string_empty();
    }

    ncc_buffer_t *ob = ncc_buffer_empty();

    size_t nt_count = ncc_list_len(g->nt_list);

    for (size_t ni = 0; ni < nt_count; ni++) {
        ncc_nonterm_t *nt = &g->nt_list.data[ni];

        if (nt->group_nt) {
            continue;
        }

        if (!nt->name.data) {
            continue;
        }

        // Skip anonymous BNF NTs.
        if (nt->name.data[0] == '$' && nt->name.data[1] == '$') {
            continue;
        }

        ncc_buffer_putc(ob, '<');
        ncc_buffer_append(ob, nt->name.data, nt->name.u8_bytes);
        ncc_buffer_putc(ob, '>');
        ncc_buffer_puts(ob, " ::=");

        size_t rule_count = nt->rule_ids.data
            ? ncc_list_len(nt->rule_ids) : 0;

        for (size_t ri = 0; ri < rule_count; ri++) {
            int32_t            rule_ix = ncc_list_get(nt->rule_ids, ri);
            ncc_parse_rule_t *rule    = ncc_get_rule(g, rule_ix);

            if (!rule) {
                continue;
            }

            if (rule->penalty_rule) {
                continue;
            }

            if (ri > 0) {
                ncc_buffer_puts(ob, "\n    |");
            }

            ncc_buffer_putc(ob, ' ');

            size_t item_count = ncc_list_len(rule->contents);

            for (size_t mi = 0; mi < item_count; mi++) {
                if (mi > 0) {
                    ncc_buffer_putc(ob, ' ');
                }

                ncc_match_t *m = &rule->contents.data[mi];
                emit_match_item(ob, g, m);
            }
        }

        ncc_buffer_putc(ob, '\n');
    }

    ncc_string_t result = ncc_string_from_raw(ob->data, (int64_t)ob->byte_len);
    ncc_free(ob->data);
    ncc_free(ob);

    if (opts.out) {
        if (result.data) {
            fputs(result.data, opts.out);
            fflush(opts.out);
            ncc_free(result.data);
        }
        return (ncc_string_t){0};
    }

    return result;
}
