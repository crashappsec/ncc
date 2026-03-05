// commander.c — Grammar-based command-line parser.
//
// Builds a formal grammar from your CLI spec, tokenizes argv,
// then parses with PWZ. Supports flags, positionals, subcommands,
// --flag=value syntax, short flag aliases, and -- separator.

#include "parse/commander.h"
#include "parse/pwz.h"
#include "lib/alloc.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static ncc_string_t
cmdr_intern(const char *s)
{
    if (!s) {
        return ncc_string_empty();
    }

    return ncc_string_from_cstr(s);
}

static ncc_cmdr_command_t *
find_command(ncc_cmdr_t *c, const char *name)
{
    if (!name || !*name) {
        return &c->root;
    }

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        if (c->root.subcommands[i].name.data
            && strcmp(c->root.subcommands[i].name.data, name) == 0) {
            return &c->root.subcommands[i];
        }
    }

    return nullptr;
}

static ncc_cmdr_flag_spec_t *
find_flag(ncc_cmdr_command_t *cmd, const char *flag_name)
{
    if (!cmd || !flag_name) {
        return nullptr;
    }

    for (int32_t i = 0; i < cmd->n_flags; i++) {
        if (cmd->flags[i].name.data
            && strcmp(cmd->flags[i].name.data, flag_name) == 0) {
            return &cmd->flags[i];
        }

        if (cmd->flags[i].short_name.data
            && strcmp(cmd->flags[i].short_name.data, flag_name) == 0) {
            return &cmd->flags[i];
        }
    }

    return nullptr;
}

// Wrap a C string as ncc_string_t for grammar API calls.
static ncc_string_t
cstr_to_rich(const char *s)
{
    return (ncc_string_t){
        .data     = (char *)s,
        .u8_bytes = s ? strlen(s) : 0,
    };
}

// ============================================================================
// Lifecycle
// ============================================================================

ncc_cmdr_t *
ncc_cmdr_new(void)
{
    return ncc_alloc(ncc_cmdr_t);
}

static void
free_command(ncc_cmdr_command_t *cmd)
{
    if (!cmd) {
        return;
    }

    for (int32_t i = 0; i < cmd->n_subcommands; i++) {
        free_command(&cmd->subcommands[i]);
    }

    // Free interned strings in flag specs.
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        ncc_free(cmd->flags[i].name.data);
        ncc_free(cmd->flags[i].short_name.data);
        ncc_free(cmd->flags[i].doc.data);
    }

    // Free interned strings in positional specs.
    for (int32_t i = 0; i < cmd->n_positionals; i++) {
        ncc_free(cmd->positionals[i].name.data);
    }

    ncc_free(cmd->name.data);
    ncc_free(cmd->doc.data);
    ncc_free(cmd->flags);
    ncc_free(cmd->positionals);
    ncc_free(cmd->subcommands);
}

void
ncc_cmdr_free(ncc_cmdr_t *c)
{
    if (!c) {
        return;
    }

    free_command(&c->root);

    if (c->grammar) {
        ncc_grammar_free(c->grammar);
    }

    ncc_free(c->name.data);
    ncc_free(c);
}

// ============================================================================
// Builder API
// ============================================================================

void
ncc_cmdr_set_name(ncc_cmdr_t *c, const char *name)
{
    if (c) {
        ncc_free(c->name.data);
        c->name = cmdr_intern(name);
    }
}

static void
ensure_flag_capacity(ncc_cmdr_command_t *cmd)
{
    if (cmd->n_flags >= cmd->flags_cap) {
        cmd->flags_cap = cmd->flags_cap ? cmd->flags_cap * 2 : 8;
        cmd->flags     = ncc_realloc(cmd->flags,
                                 (size_t)cmd->flags_cap
                                     * sizeof(ncc_cmdr_flag_spec_t));
    }
}

static void
ensure_positional_capacity(ncc_cmdr_command_t *cmd)
{
    if (cmd->n_positionals >= cmd->positionals_cap) {
        cmd->positionals_cap = cmd->positionals_cap
                                   ? cmd->positionals_cap * 2 : 4;
        cmd->positionals = ncc_realloc(cmd->positionals,
                                   (size_t)cmd->positionals_cap
                                       * sizeof(ncc_cmdr_positional_spec_t));
    }
}

static void
ensure_subcommand_capacity(ncc_cmdr_command_t *cmd)
{
    if (cmd->n_subcommands >= cmd->subcommands_cap) {
        cmd->subcommands_cap = cmd->subcommands_cap
                                   ? cmd->subcommands_cap * 2 : 4;
        cmd->subcommands = ncc_realloc(cmd->subcommands,
                                   (size_t)cmd->subcommands_cap
                                       * sizeof(ncc_cmdr_command_t));
    }
}

void
ncc_cmdr_add_command(ncc_cmdr_t *c, const char *name, const char *doc)
{
    if (!c || !name) {
        return;
    }

    ensure_subcommand_capacity(&c->root);

    ncc_cmdr_command_t *cmd = &c->root.subcommands[c->root.n_subcommands++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
ncc_cmdr_add_subcommand(ncc_cmdr_t *c, const char *parent,
                          const char *name, const char *doc)
{
    if (!c || !name) {
        return;
    }

    ncc_cmdr_command_t *pcmd = find_command(c, parent);

    if (!pcmd) {
        return;
    }

    ensure_subcommand_capacity(pcmd);

    ncc_cmdr_command_t *cmd = &pcmd->subcommands[pcmd->n_subcommands++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->name = cmdr_intern(name);
    cmd->doc  = cmdr_intern(doc);
}

void
ncc_cmdr_add_flag(ncc_cmdr_t *c, const char *command,
                    const char *flag_name, ncc_cmdr_arg_type_t type,
                    bool takes_value, const char *doc)
{
    if (!c || !flag_name) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ensure_flag_capacity(cmd);

    ncc_cmdr_flag_spec_t *f = &cmd->flags[cmd->n_flags++];
    memset(f, 0, sizeof(*f));
    f->name        = cmdr_intern(flag_name);
    f->value_type  = type;
    f->takes_value = takes_value;
    f->doc         = cmdr_intern(doc);
}

void
ncc_cmdr_add_flag_alias(ncc_cmdr_t *c, const char *command,
                          const char *flag_name, const char *alias)
{
    if (!c || !flag_name || !alias) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ncc_cmdr_flag_spec_t *f = find_flag(cmd, flag_name);

    if (!f) {
        return;
    }

    f->short_name = cmdr_intern(alias);
}

void
ncc_cmdr_add_positional(ncc_cmdr_t *c, const char *command,
                          const char *name, ncc_cmdr_arg_type_t type,
                          int min, int max)
{
    if (!c || !name) {
        return;
    }

    ncc_cmdr_command_t *cmd = find_command(c, command);

    if (!cmd) {
        return;
    }

    ensure_positional_capacity(cmd);

    ncc_cmdr_positional_spec_t *p = &cmd->positionals[cmd->n_positionals++];
    p->name = cmdr_intern(name);
    p->type = type;
    p->min  = min;
    p->max  = max;
}

// ============================================================================
// Grammar generation
// ============================================================================

static void
register_command_terminals(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd)
{
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        ncc_cmdr_flag_spec_t *f = &cmd->flags[i];
        f->terminal_id = ncc_register_terminal(c->grammar, f->name);

        if (f->short_name.data) {
            ncc_register_terminal(c->grammar, f->short_name);
        }
    }
}

static int64_t
build_flag_nt(ncc_cmdr_t *c, [[maybe_unused]] ncc_cmdr_command_t *cmd,
              ncc_cmdr_flag_spec_t *f, const char *prefix)
{
    char nt_name[256];
    snprintf(nt_name, sizeof(nt_name), "%s-flag-%s", prefix, f->name.data);

    int64_t nt_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar, cstr_to_rich(nt_name)));

    int64_t flag_tid = f->terminal_id;
    int64_t eq_tid   = c->tok_ids[NCC_CMDR_TID_EQ];
    int64_t word_tid = c->tok_ids[NCC_CMDR_TID_WORD];
    int64_t int_tid  = c->tok_ids[NCC_CMDR_TID_INT];
    int64_t flt_tid  = c->tok_ids[NCC_CMDR_TID_FLOAT];
    int64_t bool_tid = c->tok_ids[NCC_CMDR_TID_BOOL];

    if (f->takes_value) {
        // --flag=value
        ncc_add_rule_v(c->grammar, nt_id, 3,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid),
                            NCC_TERMINAL(eq_tid),
                            NCC_TERMINAL(word_tid)});

        if (f->short_name.data) {
            int64_t short_tid = ncc_register_terminal(
                c->grammar, f->short_name);
            ncc_add_rule_v(c->grammar, nt_id, 3,
                (ncc_match_t[]){NCC_TERMINAL(short_tid),
                                NCC_TERMINAL(eq_tid),
                                NCC_TERMINAL(word_tid)});
            ncc_add_rule_v(c->grammar, nt_id, 2,
                (ncc_match_t[]){NCC_TERMINAL(short_tid),
                                NCC_TERMINAL(word_tid)});
        }

        // --flag value
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid),
                            NCC_TERMINAL(word_tid)});

        // Also accept int/float/bool tokens as values.
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(int_tid)});
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(flt_tid)});
        ncc_add_rule_v(c->grammar, nt_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid), NCC_TERMINAL(bool_tid)});
    }
    else {
        // Boolean flag (no value).
        ncc_add_rule_v(c->grammar, nt_id, 1,
            (ncc_match_t[]){NCC_TERMINAL(flag_tid)});

        if (f->short_name.data) {
            int64_t short_tid = ncc_register_terminal(
                c->grammar, f->short_name);
            ncc_add_rule_v(c->grammar, nt_id, 1,
                (ncc_match_t[]){NCC_TERMINAL(short_tid)});
        }
    }

    return nt_id;
}

static int64_t
build_items_nt(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd, const char *prefix)
{
    char nt_name[256];
    snprintf(nt_name, sizeof(nt_name), "%s-items", prefix);

    int64_t items_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar, cstr_to_rich(nt_name)));

    ncc_match_t items_nt = {.kind = NCC_MATCH_NT, .nt_id = items_id};

    // items -> ""
    ncc_add_rule_v(c->grammar, items_id, 1,
        (ncc_match_t[]){NCC_EPSILON()});

    // items -> flag items  (for each flag)
    for (int32_t i = 0; i < cmd->n_flags; i++) {
        int64_t fnt_id = build_flag_nt(c, cmd, &cmd->flags[i], prefix);
        ncc_add_rule_v(c->grammar, items_id, 2,
            (ncc_match_t[]){{.kind = NCC_MATCH_NT, .nt_id = fnt_id},
                            items_nt});
    }

    // items -> WORD items | INT items | FLOAT items | BOOL items
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_WORD]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_INT]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_FLOAT]),
                        items_nt});
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_BOOL]),
                        items_nt});

    // items -> FLAG items  (unknown flags become positional args)
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_FLAG]),
                        items_nt});

    // items -> DD items  (-- separator)
    ncc_add_rule_v(c->grammar, items_id, 2,
        (ncc_match_t[]){NCC_TERMINAL(c->tok_ids[NCC_CMDR_TID_DD]),
                        items_nt});

    return items_id;
}

static void
build_command_grammar(ncc_cmdr_t *c, ncc_cmdr_command_t *cmd,
                      int64_t parent_id, const char *prefix)
{
    int64_t items_id = build_items_nt(c, cmd, prefix);
    ncc_match_t items_nt = {.kind = NCC_MATCH_NT, .nt_id = items_id};

    if (cmd->name.data) {
        int64_t name_tid = ncc_register_terminal(c->grammar, cmd->name);
        ncc_add_rule_v(c->grammar, parent_id, 2,
            (ncc_match_t[]){NCC_TERMINAL(name_tid), items_nt});
    }
    else {
        ncc_add_rule_v(c->grammar, parent_id, 1,
            (ncc_match_t[]){items_nt});
    }

    for (int32_t i = 0; i < cmd->n_subcommands; i++) {
        ncc_cmdr_command_t *sub = &cmd->subcommands[i];
        char sub_prefix[256];
        snprintf(sub_prefix, sizeof(sub_prefix), "%s-%s",
                 prefix, sub->name.data);
        build_command_grammar(c, sub, parent_id, sub_prefix);
    }
}

void
ncc_cmdr_finalize(ncc_cmdr_t *c)
{
    if (!c || c->finalized) {
        return;
    }

    c->grammar = ncc_grammar_new();
    ncc_grammar_set_error_recovery(c->grammar, false);

    // Register base token types.
    c->tok_ids[NCC_CMDR_TID_WORD]  = ncc_register_terminal(
        c->grammar, cstr_to_rich("WORD"));
    c->tok_ids[NCC_CMDR_TID_INT]   = ncc_register_terminal(
        c->grammar, cstr_to_rich("INT"));
    c->tok_ids[NCC_CMDR_TID_FLOAT] = ncc_register_terminal(
        c->grammar, cstr_to_rich("FLOAT"));
    c->tok_ids[NCC_CMDR_TID_BOOL]  = ncc_register_terminal(
        c->grammar, cstr_to_rich("BOOL"));
    c->tok_ids[NCC_CMDR_TID_EQ]    = ncc_register_terminal(
        c->grammar, cstr_to_rich("EQ"));
    c->tok_ids[NCC_CMDR_TID_COMMA] = ncc_register_terminal(
        c->grammar, cstr_to_rich("COMMA"));
    c->tok_ids[NCC_CMDR_TID_DD]    = ncc_register_terminal(
        c->grammar, cstr_to_rich("DD"));
    c->tok_ids[NCC_CMDR_TID_FLAG]  = ncc_register_terminal(
        c->grammar, cstr_to_rich("FLAG"));

    // Register all flag and subcommand terminals.
    register_command_terminals(c, &c->root);

    for (int32_t i = 0; i < c->root.n_subcommands; i++) {
        register_command_terminals(c, &c->root.subcommands[i]);
        ncc_register_terminal(c->grammar, c->root.subcommands[i].name);
    }

    // Build grammar.
    int64_t start_id = ncc_nonterm_id(
        ncc_nonterm(c->grammar, cstr_to_rich("cmd")));
    ncc_grammar_set_start_id(c->grammar, start_id);

    if (c->root.n_subcommands > 0) {
        for (int32_t i = 0; i < c->root.n_subcommands; i++) {
            ncc_cmdr_command_t *sub = &c->root.subcommands[i];
            char prefix[256];
            snprintf(prefix, sizeof(prefix), "cmd-%s", sub->name.data);
            build_command_grammar(c, sub, start_id, prefix);
        }

        if (c->root.n_flags > 0) {
            build_command_grammar(c, &c->root, start_id, "cmd-root");
        }
    }
    else {
        build_command_grammar(c, &c->root, start_id, "cmd");
    }

    ncc_grammar_finalize(c->grammar);
    c->finalized = true;
}

// ============================================================================
// Tokenizer
// ============================================================================

static bool
is_int_str(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    const char *p = s;

    if (*p == '-' || *p == '+') {
        p++;
    }

    if (!*p) {
        return false;
    }

    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        p++;
    }

    return true;
}

static bool
is_float_str(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    char *end = nullptr;
    strtod(s, &end);

    return end && *end == '\0' && end != s
           && (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E'));
}

static bool
is_bool_str(const char *s)
{
    if (!s) {
        return false;
    }

    return strcmp(s, "true") == 0 || strcmp(s, "false") == 0
        || strcmp(s, "yes") == 0 || strcmp(s, "no") == 0;
}

static int64_t
find_flag_tid(ncc_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return 0;
    }

    ncc_cmdr_command_t *root = &c->root;

    for (int32_t i = 0; i < root->n_flags; i++) {
        if (root->flags[i].name.data
            && strcmp(root->flags[i].name.data, name) == 0) {
            return root->flags[i].terminal_id;
        }

        if (root->flags[i].short_name.data
            && strcmp(root->flags[i].short_name.data, name) == 0) {
            return root->flags[i].terminal_id;
        }
    }

    for (int32_t si = 0; si < root->n_subcommands; si++) {
        ncc_cmdr_command_t *sub = &root->subcommands[si];

        for (int32_t i = 0; i < sub->n_flags; i++) {
            if (sub->flags[i].name.data
                && strcmp(sub->flags[i].name.data, name) == 0) {
                return sub->flags[i].terminal_id;
            }

            if (sub->flags[i].short_name.data
                && strcmp(sub->flags[i].short_name.data, name) == 0) {
                return sub->flags[i].terminal_id;
            }
        }
    }

    return 0;
}

static bool
flag_takes_value(ncc_cmdr_t *c, const char *name)
{
    if (!c || !name) {
        return false;
    }

    ncc_cmdr_command_t *root = &c->root;

    for (int32_t i = 0; i < root->n_flags; i++) {
        if (root->flags[i].name.data
            && strcmp(root->flags[i].name.data, name) == 0) {
            return root->flags[i].takes_value;
        }

        if (root->flags[i].short_name.data
            && strcmp(root->flags[i].short_name.data, name) == 0) {
            return root->flags[i].takes_value;
        }
    }

    for (int32_t si = 0; si < root->n_subcommands; si++) {
        ncc_cmdr_command_t *sub = &root->subcommands[si];

        for (int32_t i = 0; i < sub->n_flags; i++) {
            if (sub->flags[i].name.data
                && strcmp(sub->flags[i].name.data, name) == 0) {
                return sub->flags[i].takes_value;
            }

            if (sub->flags[i].short_name.data
                && strcmp(sub->flags[i].short_name.data, name) == 0) {
                return sub->flags[i].takes_value;
            }
        }
    }

    return false;
}

static int64_t
classify_word(ncc_cmdr_t *c, const char *s)
{
    if (is_int_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_INT];
    }

    if (is_float_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_FLOAT];
    }

    if (is_bool_str(s)) {
        return c->tok_ids[NCC_CMDR_TID_BOOL];
    }

    return c->tok_ids[NCC_CMDR_TID_WORD];
}

typedef struct {
    ncc_token_info_t **tokens;
    int32_t             len;
    int32_t             cap;
} tok_list_t;

static void
tok_list_init(tok_list_t *tl)
{
    tl->cap    = 32;
    tl->len    = 0;
    tl->tokens = ncc_alloc_array(ncc_token_info_t *, (size_t)tl->cap);
}

static void
tok_list_push(tok_list_t *tl, ncc_token_info_t *tok)
{
    if (tl->len >= tl->cap) {
        tl->cap *= 2;
        tl->tokens = ncc_realloc(tl->tokens,
                              (size_t)tl->cap * sizeof(ncc_token_info_t *));
    }

    tl->tokens[tl->len++] = tok;
}

static ncc_token_info_t *
make_token(const char *value, int64_t tid, int32_t index)
{
    ncc_token_info_t *tok = ncc_alloc(ncc_token_info_t);

    if (value && *value) {
        ncc_string_t s = cmdr_intern(value);
        tok->value = ncc_option_set(ncc_string_t, s);
    }

    tok->tid    = (int32_t)tid;
    tok->index  = index;
    tok->line   = 1;
    tok->column = 1;

    return tok;
}

static int32_t
cmdr_tokenize(const char **argv, int argc, ncc_cmdr_t *c,
              ncc_token_info_t ***tokens_out, int32_t *n_tokens_out)
{
    if (!argv || argc <= 0 || !tokens_out || !n_tokens_out || !c) {
        return -1;
    }

    tok_list_t tl;
    tok_list_init(&tl);

    bool past_dd = false;

    for (int i = 0; i < argc; i++) {
        const char *arg = argv[i];

        if (!arg) {
            continue;
        }

        // -- means end of flags.
        if (!past_dd && strcmp(arg, "--") == 0) {
            past_dd = true;
            tok_list_push(&tl, make_token("--",
                                           c->tok_ids[NCC_CMDR_TID_DD],
                                           tl.len));
            continue;
        }

        // After --, everything is a word.
        if (past_dd) {
            tok_list_push(&tl, make_token(arg, classify_word(c, arg),
                                           tl.len));
            continue;
        }

        // --flag=value
        if (strncmp(arg, "--", 2) == 0 && strlen(arg) > 2) {
            const char *eq = strchr(arg, '=');

            if (eq) {
                size_t flag_len = (size_t)(eq - arg);
                char *flag_name = ncc_alloc_size(1, flag_len + 1);
                memcpy(flag_name, arg, flag_len);

                int64_t ftid = find_flag_tid(c, flag_name);

                if (!ftid) {
                    ftid = c->tok_ids[NCC_CMDR_TID_FLAG];
                }

                tok_list_push(&tl, make_token(flag_name, ftid, tl.len));
                ncc_free(flag_name);

                tok_list_push(&tl, make_token("=",
                                               c->tok_ids[NCC_CMDR_TID_EQ],
                                               tl.len));

                const char *val = eq + 1;
                tok_list_push(&tl, make_token(val, classify_word(c, val),
                                               tl.len));
            }
            else {
                int64_t ftid = find_flag_tid(c, arg);

                if (!ftid) {
                    ftid = c->tok_ids[NCC_CMDR_TID_FLAG];
                }

                tok_list_push(&tl, make_token(arg, ftid, tl.len));
            }

            continue;
        }

        // -x short flag
        if (arg[0] == '-' && arg[1] != '\0'
            && !isdigit((unsigned char)arg[1])) {
            int64_t ftid = find_flag_tid(c, arg);

            if (ftid) {
                tok_list_push(&tl, make_token(arg, ftid, tl.len));
                continue;
            }

            // Try splitting combined short flags (-abc -> -a -b -c).
            bool can_split = false;

            if (strlen(arg) > 2) {
                can_split = true;

                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, short_flag);

                    if (!stid || flag_takes_value(c, short_flag)) {
                        can_split = false;
                        break;
                    }
                }
            }

            if (can_split) {
                for (int j = 1; arg[j]; j++) {
                    char    short_flag[3] = {'-', arg[j], '\0'};
                    int64_t stid = find_flag_tid(c, short_flag);
                    tok_list_push(&tl, make_token(short_flag, stid, tl.len));
                }
            }
            else {
                tok_list_push(&tl, make_token(arg,
                                               c->tok_ids[NCC_CMDR_TID_FLAG],
                                               tl.len));
            }

            continue;
        }

        // Bare word (may be a subcommand name).
        int64_t tid = classify_word(c, arg);

        for (int32_t si = 0; si < c->root.n_subcommands; si++) {
            if (c->root.subcommands[si].name.data
                && strcmp(arg, c->root.subcommands[si].name.data) == 0) {
                tid = ncc_register_terminal(c->grammar,
                                              cstr_to_rich(arg));
                break;
            }
        }

        tok_list_push(&tl, make_token(arg, tid, tl.len));
    }

    // Add EOF.
    tok_list_push(&tl, make_token("", NCC_TOK_EOF, tl.len));

    *tokens_out   = tl.tokens;
    *n_tokens_out = tl.len;

    return 0;
}

// ============================================================================
// Parsing
// ============================================================================

static ncc_cmdr_result_t *
make_error_result(const char *msg)
{
    ncc_cmdr_result_t *r = ncc_alloc(ncc_cmdr_result_t);
    r->ok       = false;
    r->errors   = ncc_alloc(ncc_string_t);
    r->errors[0] = cmdr_intern(msg);
    r->n_errors = 1;

    return r;
}

// Get token text as a C string from a parse tree node.
static const char *
get_token_text(ncc_parse_tree_t *node)
{
    if (!node || !ncc_tree_is_leaf(node)) {
        return nullptr;
    }

    ncc_token_info_t *tok = ncc_tree_leaf_value(node);

    if (!tok || !ncc_option_is_set(tok->value)) {
        return nullptr;
    }

    ncc_string_t val = ncc_option_get(tok->value);
    return val.data;
}

// Recursively collect all terminal text from a parse tree.
static void
collect_terminal_text(ncc_parse_tree_t *tree, const char ***texts,
                      int32_t *n, int32_t *cap)
{
    if (!tree) {
        return;
    }

    if (ncc_tree_is_leaf(tree)) {
        const char *text = get_token_text(tree);

        if (text && *text) {
            if (*n >= *cap) {
                *cap = *cap ? *cap * 2 : 16;
                *texts = ncc_realloc(*texts, (size_t)*cap * sizeof(const char *));
            }

            (*texts)[(*n)++] = text;
        }

        return;
    }

    size_t nc = ncc_tree_num_children(tree);

    for (size_t i = 0; i < nc; i++) {
        collect_terminal_text(ncc_tree_child(tree, i), texts, n, cap);
    }
}

static void
extract_result_from_tree(ncc_cmdr_t *c, ncc_parse_tree_t *tree,
                          ncc_cmdr_result_t *r)
{
    if (!tree || !r) {
        return;
    }

    const char **texts = nullptr;
    int32_t n   = 0;
    int32_t cap = 0;

    collect_terminal_text(tree, &texts, &n, &cap);

    int32_t args_cap = 0;
    bool    past_dd  = false;

    for (int32_t i = 0; i < n; i++) {
        const char *text = texts[i];

        if (!text || !*text) {
            continue;
        }

        if (strcmp(text, "--") == 0) {
            past_dd = true;
            goto next;
        }

        if (!r->command.data && !past_dd) {
            for (int32_t si = 0; si < c->root.n_subcommands; si++) {
                if (c->root.subcommands[si].name.data
                    && strcmp(text, c->root.subcommands[si].name.data) == 0) {
                    r->command = cmdr_intern(text);
                    goto next;
                }
            }
        }

        if (past_dd) {
            goto add_arg;
        }

        // Check if this is a flag.
        {
            ncc_cmdr_command_t *cmd = r->command.data
                ? find_command(c, r->command.data) : &c->root;

            if (!cmd) {
                cmd = &c->root;
            }

            ncc_cmdr_flag_spec_t *flag = find_flag(cmd, text);

            if (!flag) {
                flag = find_flag(&c->root, text);
            }

            if (flag) {
                ncc_cmdr_val_t *v = ncc_alloc(ncc_cmdr_val_t);

                if (flag->takes_value && i + 1 < n) {
                    if (i + 1 < n && strcmp(texts[i + 1], "=") == 0) {
                        i++;
                    }

                    if (i + 1 < n) {
                        i++;
                        const char *val = texts[i];

                        switch (flag->value_type) {
                        case NCC_CMDR_TYPE_INT:
                            v->tag = NCC_CMDR_VAL_INT;
                            v->i   = strtoll(val, nullptr, 10);
                            break;
                        case NCC_CMDR_TYPE_FLOAT:
                            v->tag = NCC_CMDR_VAL_FLOAT;
                            v->f   = strtod(val, nullptr);
                            break;
                        case NCC_CMDR_TYPE_BOOL:
                            v->tag = NCC_CMDR_VAL_BOOL;
                            v->b   = (strcmp(val, "true") == 0
                                      || strcmp(val, "yes") == 0);
                            break;
                        default:
                            v->tag = NCC_CMDR_VAL_STR;
                            v->s   = ncc_string_from_cstr(val);
                            break;
                        }
                    }
                    else {
                        ncc_free(v);
                        goto next;
                    }
                }
                else {
                    v->tag = NCC_CMDR_VAL_BOOL;
                    v->b   = true;
                }

                // Store under long flag name.
                _ncc_dict_put(&r->flags,
                                       (void *)strdup(flag->name.data),
                                       (void *)v);

                // Also store under alias.
                if (flag->short_name.data) {
                    ncc_cmdr_val_t *alias_v = ncc_alloc(ncc_cmdr_val_t);
                    *alias_v = *v;
                    if (alias_v->tag == NCC_CMDR_VAL_STR && alias_v->s.data) {
                        alias_v->s = ncc_string_from_cstr(alias_v->s.data);
                    }
                    _ncc_dict_put(
                        &r->flags,
                        (void *)strdup(flag->short_name.data),
                        (void *)alias_v);
                }

                goto next;
            }
        }

        if (strcmp(text, "=") == 0) {
            goto next;
        }

    add_arg:
        if (r->n_args >= args_cap) {
            args_cap = args_cap ? args_cap * 2 : 8;
            r->args = ncc_realloc(r->args,
                               (size_t)args_cap * sizeof(ncc_cmdr_arg_t));
        }

        {
            ncc_cmdr_arg_t *arg = &r->args[r->n_args++];
            arg->value     = ncc_string_from_cstr(text);
            arg->int_val   = strtoll(text, nullptr, 10);
            arg->float_val = strtod(text, nullptr);
        }

    next:;
    }

    ncc_free(texts);
}

static void
free_tokens(ncc_token_info_t **tokens, int32_t n_tokens)
{
    if (!tokens) {
        return;
    }

    for (int32_t i = 0; i < n_tokens; i++) {
        if (tokens[i]) {
            if (ncc_option_is_set(tokens[i]->value)) {
                ncc_string_t val = ncc_option_get(tokens[i]->value);
                ncc_free(val.data);
            }
            ncc_free(tokens[i]);
        }
    }

    ncc_free(tokens);
}

ncc_cmdr_result_t *
ncc_cmdr_parse(ncc_cmdr_t *c, int argc, const char **argv)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        ncc_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    ncc_token_info_t **tokens = nullptr;
    int32_t             n_tokens = 0;

    if (cmdr_tokenize(argv, argc, c, &tokens, &n_tokens) < 0) {
        return make_error_result("tokenization failed");
    }

    // Parse using PWZ with a token stream from the array.
    ncc_token_stream_t *ts = ncc_token_stream_from_array(tokens, n_tokens);
    ncc_pwz_parser_t   *pp = ncc_pwz_new(c->grammar);
    bool parse_ok = ncc_pwz_parse(pp, ts);

    ncc_cmdr_result_t *r = ncc_alloc(ncc_cmdr_result_t);
    ncc_dict_init(&r->flags, ncc_hash_cstring, ncc_dict_cstr_eq);

    if (!parse_ok) {
        r->ok       = false;
        r->errors   = ncc_alloc(ncc_string_t);
        r->errors[0] = cmdr_intern("parse failed");
        r->n_errors = 1;
    }
    else {
        r->ok   = true;
        r->tree = ncc_pwz_get_tree(pp);

        if (r->tree) {
            extract_result_from_tree(c, r->tree, r);
        }
    }

    ncc_pwz_free(pp);
    ncc_token_stream_free(ts);
    free_tokens(tokens, n_tokens);

    return r;
}

ncc_cmdr_result_t *
ncc_cmdr_parse_string(ncc_cmdr_t *c, const char *cmdline)
{
    if (!c) {
        return make_error_result("commander not initialized");
    }

    if (!c->finalized) {
        ncc_cmdr_finalize(c);
    }

    if (!c->grammar) {
        return make_error_result("grammar not available");
    }

    // Split cmdline into argv.
    const char **argv = nullptr;
    int          argc = 0;
    int          cap  = 16;
    argv = ncc_alloc_array(const char *, (size_t)cap);

    const char *p = cmdline;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        if (!*p) {
            break;
        }

        const char *start = p;
        bool in_quote      = false;
        char quote_char    = 0;

        while (*p) {
            if (in_quote) {
                if (*p == quote_char) {
                    in_quote = false;
                }
                p++;
            }
            else if (*p == '"' || *p == '\'') {
                in_quote   = true;
                quote_char = *p;
                p++;
            }
            else if (isspace((unsigned char)*p)) {
                break;
            }
            else {
                p++;
            }
        }

        size_t len = (size_t)(p - start);

        if (len >= 2
            && ((start[0] == '"' && start[len - 1] == '"')
                || (start[0] == '\'' && start[len - 1] == '\''))) {
            start++;
            len -= 2;
        }

        char *word = ncc_alloc_size(1, len + 1);
        memcpy(word, start, len);

        if (argc >= cap) {
            cap *= 2;
            argv = ncc_realloc(argv, (size_t)cap * sizeof(const char *));
        }

        argv[argc++] = word;
    }

    ncc_cmdr_result_t *result = ncc_cmdr_parse(c, argc, argv);

    for (int i = 0; i < argc; i++) {
        ncc_free((void *)argv[i]);
    }
    ncc_free(argv);

    return result;
}

// ============================================================================
// Result queries
// ============================================================================

const char *
ncc_cmdr_result_command(ncc_cmdr_result_t *r)
{
    return r ? r->command.data : nullptr;
}

bool
ncc_cmdr_flag_present(ncc_cmdr_result_t *r, const char *flag)
{
    if (!r || !flag) {
        return false;
    }

    return ncc_dict_contains(&r->flags, (void *)flag);
}

ncc_cmdr_val_t *
ncc_cmdr_flag_get(ncc_cmdr_result_t *r, const char *flag)
{
    if (!r || !flag) {
        return nullptr;
    }

    bool found;
    void *val = ncc_dict_get(&r->flags, (void *)flag, &found);

    return found ? (ncc_cmdr_val_t *)val : nullptr;
}

const char *
ncc_cmdr_flag_str(ncc_cmdr_result_t *r, const char *flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_STR) {
        return nullptr;
    }

    return v->s.data;
}

int64_t
ncc_cmdr_flag_int(ncc_cmdr_result_t *r, const char *flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_INT) {
        return 0;
    }

    return v->i;
}

bool
ncc_cmdr_flag_bool(ncc_cmdr_result_t *r, const char *flag)
{
    ncc_cmdr_val_t *v = ncc_cmdr_flag_get(r, flag);

    if (!v || v->tag != NCC_CMDR_VAL_BOOL) {
        return false;
    }

    return v->b;
}

int32_t
ncc_cmdr_arg_count(ncc_cmdr_result_t *r)
{
    return r ? r->n_args : 0;
}

const char *
ncc_cmdr_arg_str(ncc_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= r->n_args) {
        return nullptr;
    }

    return r->args[index].value.data;
}

int64_t
ncc_cmdr_arg_int(ncc_cmdr_result_t *r, int index)
{
    if (!r || index < 0 || index >= r->n_args) {
        return 0;
    }

    return r->args[index].int_val;
}

// ============================================================================
// Result cleanup
// ============================================================================

void
ncc_cmdr_result_free(ncc_cmdr_result_t *r)
{
    if (!r) {
        return;
    }

    ncc_free(r->command.data);

    // Free interned keys and value structs in the flags dict.
    for (size_t i = 0; i < r->flags.capacity; i++) {
        if (r->flags.buckets[i].state != _NCC_BUCKET_OCCUPIED) {
            continue;
        }
        ncc_free(r->flags.buckets[i].key);
        ncc_cmdr_val_t *v = (ncc_cmdr_val_t *)r->flags.buckets[i].value;
        if (v) {
            if (v->tag == NCC_CMDR_VAL_STR) {
                ncc_free(v->s.data);
            }
            ncc_free(v);
        }
    }

    ncc_dict_free(&r->flags);

    for (int32_t i = 0; i < r->n_args; i++) {
        ncc_free(r->args[i].value.data);
    }

    ncc_free(r->args);

    for (int32_t i = 0; i < r->n_errors; i++) {
        ncc_free(r->errors[i].data);
    }

    ncc_free(r->errors);

    // Parse tree nodes are arena-allocated (ncc_alloc); we just
    // clear the pointer to avoid stale access.
    r->tree = nullptr;

    ncc_free(r);
}

// ============================================================================
// Error output
// ============================================================================

int32_t
ncc_cmdr_print_errors(ncc_cmdr_result_t *r, FILE *out)
{
    if (!r || r->ok || !r->errors) {
        return 0;
    }

    for (int32_t i = 0; i < r->n_errors; i++) {
        fprintf(out, "error: %s\n", r->errors[i].data);
    }

    return r->n_errors;
}
