// type_infer.c — Expression type inference over the scoped symbol table.
//
// Returns the canonical type spelling of an expression (e.g. "n00b_string_t*").
// Coverage is incremental: identifier, parenthesization, cast, address-of, and
// dereference are handled today; member access, calls, subscripts, and
// arithmetic return nullptr (callers fall back rather than emit a wrong type).
//
// The spelling is built so it matches ncc_normalize_type_tree's output for the
// same type written explicitly: a normalized base (from the declaration
// specifiers) followed by pointer stars with no intervening space. That makes
// the result usable both as a typeof replacement and as a typehash/typeid key.

#include "parse/type_infer.h"
#include "util/type_normalize.h"
#include "lib/alloc.h"
#include "lib/buffer.h"
#include <string.h>

// ============================================================================
// Tree helpers (parse-layer)
// ============================================================================

static bool
nt_is(ncc_parse_tree_t *node, const char *lit)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return false;
    }
    ncc_string_t name = ncc_tree_node_value(node).name;
    return name.data && strcmp(name.data, lit) == 0;
}

static bool
is_group(ncc_parse_tree_t *node)
{
    return node && !ncc_tree_is_leaf(node) && ncc_tree_node_value(node).group_top;
}

static ncc_parse_tree_t *
child_nt(ncc_parse_tree_t *node, const char *child_name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (!c || ncc_tree_is_leaf(c)) {
            continue;
        }
        if (is_group(c)) {
            ncc_parse_tree_t *f = child_nt(c, child_name);
            if (f) {
                return f;
            }
            continue;
        }
        ncc_string_t nm = ncc_tree_node_value(c).name;
        if (nm.data && strcmp(nm.data, child_name) == 0) {
            return c;
        }
    }
    return nullptr;
}

// Leaf token text of a leaf node (empty if not a value leaf).
static ncc_string_t
leaf_text(ncc_parse_tree_t *node)
{
    if (node && ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && ncc_option_is_set(tok->value)) {
            return ncc_option_get(tok->value);
        }
    }
    return ncc_string_empty();
}

// Count the non-trivial (nonterminal or value-leaf) children of a node,
// returning the single such child via *only when there is exactly one.
static size_t
meaningful_children(ncc_parse_tree_t *node, ncc_parse_tree_t **only)
{
    size_t            count = 0;
    ncc_parse_tree_t *last  = nullptr;
    size_t            nc    = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *c = ncc_tree_child(node, i);
        if (!c) {
            continue;
        }
        count++;
        last = c;
    }
    if (only) {
        *only = (count == 1) ? last : nullptr;
    }
    return count;
}

// Pointer depth contributed by a declarator's explicit stars, not descending
// into parameter lists. Returns -1 for forms we cannot type simply (arrays,
// function declarators) so the caller bails.
static int
declarator_ptr_depth(ncc_parse_tree_t *node)
{
    if (!node) {
        return 0;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_string_t t = leaf_text(node);
        return (t.data && strcmp(t.data, "*") == 0) ? 1 : 0;
    }
    if (nt_is(node, "parameter_type_list") || nt_is(node, "parameter_list")) {
        return 0;
    }
    if (nt_is(node, "array_declarator")) {
        return -1; // arrays not handled by the simple spelling yet
    }
    // A nested function declarator (function pointer) — bail.
    if (child_nt(node, "parameter_type_list") || child_nt(node, "parameter_list")) {
        // Only bail if this isn't the top call form; conservatively bail.
        return -1;
    }
    int total = 0;
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        int d = declarator_ptr_depth(ncc_tree_child(node, i));
        if (d < 0) {
            return -1;
        }
        total += d;
    }
    return total;
}

// Storage-class and function specifiers are not part of a value's type; drop
// them from a normalized specifier spelling (keep const/volatile, which are).
static bool
is_storage_word(const char *w, size_t len)
{
    static const char *const kw[] = {
        "static",    "extern",       "register",  "auto",
        "typedef",   "thread_local", "constexpr", "inline",
        "_Noreturn", "__inline",     "__inline__",
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (strlen(kw[i]) == len && strncmp(w, kw[i], len) == 0) {
            return true;
        }
    }
    return false;
}

// Remove storage-class words from a space-joined normalized type spelling.
// Returns a fresh string (frees the input).
static char *
strip_storage_classes(char *base)
{
    if (!base) {
        return nullptr;
    }
    ncc_buffer_t *b   = ncc_buffer_empty();
    const char   *p   = base;
    bool          any = false;
    while (*p) {
        const char *start = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t wlen = (size_t)(p - start);
        if (wlen > 0 && !is_storage_word(start, wlen)) {
            ncc_buffer_printf(b, "%s%.*s", any ? " " : "", (int)wlen, start);
            any = true;
        }
        while (*p == ' ') {
            p++;
        }
    }
    ncc_free(base);
    return ncc_buffer_take(b);
}

// Canonical base spelling from a specifier subtree, with any trailing declared
// name stripped (the same pwz ambiguity that folds `x` into `int x` specifiers
// can apply here; the canonicalizer space-joins alnum tokens, so a trailing
// " <name>" is removable). Caller frees.
static char *
canonical_base(ncc_parse_tree_t *specs, ncc_string_t strip_name)
{
    ncc_string_t norm = ncc_normalize_type_tree(specs);
    if (!norm.data) {
        return nullptr;
    }
    if (strip_name.data && strip_name.u8_bytes > 0) {
        size_t blen = strlen(norm.data);
        size_t flen = strip_name.u8_bytes;
        if (blen > flen + 1 && norm.data[blen - flen - 1] == ' '
            && strncmp(norm.data + blen - flen, strip_name.data, flen) == 0
            && norm.data[blen] == '\0') {
            norm.data[blen - flen - 1] = '\0';
        }
    }
    return strip_storage_classes(norm.data);
}

static char *
append_stars(char *base, int n)
{
    if (!base || n <= 0) {
        return base;
    }
    ncc_buffer_t *b = ncc_buffer_empty();
    ncc_buffer_puts(b, base);
    for (int i = 0; i < n; i++) {
        ncc_buffer_puts(b, "*");
    }
    ncc_free(base);
    return ncc_buffer_take(b);
}

// Drop one trailing '*' from a pointer type spelling; nullptr if not a pointer.
static char *
strip_one_star(char *type)
{
    if (!type) {
        return nullptr;
    }
    size_t len = strlen(type);
    while (len > 0 && (type[len - 1] == ' ' || type[len - 1] == '\t')) {
        len--;
    }
    if (len == 0 || type[len - 1] != '*') {
        ncc_free(type);
        return nullptr;
    }
    type[len - 1] = '\0';
    // Trim a trailing space left before the removed star.
    size_t n = strlen(type);
    while (n > 0 && (type[n - 1] == ' ' || type[n - 1] == '\t')) {
        type[--n] = '\0';
    }
    return type;
}

// ============================================================================
// Symbol -> type spelling
// ============================================================================

static char *
type_of_symbol(ncc_sym_entry_t *sym)
{
    if (!sym || !sym->type_node) {
        return nullptr;
    }
    int depth = declarator_ptr_depth(sym->decl_node);
    if (depth < 0) {
        return nullptr; // array / function declarator — not yet typed
    }
    char *base = canonical_base(sym->type_node, sym->name);
    if (!base) {
        return nullptr;
    }
    return append_stars(base, depth);
}

// ============================================================================
// Aggregate / member resolution
// ============================================================================

// First descendant (incl. self) with NT name `name`.
static ncc_parse_tree_t *
find_descendant_nt(ncc_parse_tree_t *node, const char *name)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }
    if (nt_is(node, name)) {
        return node;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *f = find_descendant_nt(ncc_tree_child(node, i), name);
        if (f) {
            return f;
        }
    }
    return nullptr;
}

// Text of the first IDENTIFIER/TYPEDEF_NAME leaf at or under `node`.
static ncc_string_t
identifier_text(ncc_parse_tree_t *node)
{
    if (!node) {
        return ncc_string_empty();
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok
            && (tok->tid == NCC_TOK_IDENTIFIER
                || tok->tid == NCC_TOK_TYPEDEF_NAME)) {
            return leaf_text(node);
        }
        return ncc_string_empty();
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        ncc_string_t t = identifier_text(ncc_tree_child(node, i));
        if (t.data && t.u8_bytes > 0) {
            return t;
        }
    }
    return ncc_string_empty();
}

// Last IDENTIFIER leaf in DFS order under a member_declaration — its declared
// field name — skipping nested aggregate bodies and parameter lists (whose
// identifiers belong to inner declarations, not this field). Works whether the
// name sits in the declarator (`T *x`) or folds into the specifiers (`T x`).
static void
last_field_identifier(ncc_parse_tree_t *node, ncc_string_t *out)
{
    if (!node) {
        return;
    }
    if (ncc_tree_is_leaf(node)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(node);
        if (tok && tok->tid == NCC_TOK_IDENTIFIER) {
            *out = leaf_text(node);
        }
        return;
    }
    if (nt_is(node, "member_declaration_list") || nt_is(node, "parameter_type_list")
        || nt_is(node, "parameter_list")) {
        return;
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        last_field_identifier(ncc_tree_child(node, i), out);
    }
}

// Resolve a type name to its aggregate specifier (one carrying a member body)
// via the scoped symbol table, following a typedef to its underlying tag. A
// leading struct/union/enum keyword (a tag-reference spelling) is handled.
// Catches types the legacy aggregate table misses (notably _generic_struct
// typedefs). Returns a specifier with a member_declaration_list, or nullptr.
ncc_parse_tree_t *
ncc_symtab_aggregate_spec(ncc_symtab_t *st, const char *type_name)
{
    if (!st || !type_name) {
        return nullptr;
    }

    // A tag-reference spelling ("struct foo") resolves directly in the tag ns
    // by its bare tag name.
    const char *bare = type_name;
    for (const char *kw = nullptr;;) {
        kw = nullptr;
        if (strncmp(bare, "struct ", 7) == 0) {
            kw = bare + 7;
        }
        else if (strncmp(bare, "union ", 6) == 0) {
            kw = bare + 6;
        }
        else if (strncmp(bare, "enum ", 5) == 0) {
            kw = bare + 5;
        }
        if (!kw) {
            break;
        }
        while (*kw == ' ') {
            kw++;
        }
        bare = kw;
    }

    ncc_string_t      nm     = ncc_string_from_cstr(type_name);
    ncc_string_t      barenm = ncc_string_from_cstr(bare);
    ncc_parse_tree_t *result = nullptr;

    ncc_sym_entry_t *sym = ncc_symtab_lookup(st, ncc_string_empty(), nm);
    if (sym && sym->kind == NCC_SYM_TYPEDEF && sym->type_node) {
        ncc_parse_tree_t *su = find_descendant_nt(sym->type_node,
                                                  "struct_or_union_specifier");
        if (su && child_nt(su, "member_declaration_list")) {
            result = su;
        }
        else if (su) {
            ncc_parse_tree_t *tagn = child_nt(su, "tag_name");
            if (tagn) {
                ncc_string_t     tag = identifier_text(tagn);
                ncc_sym_entry_t *te  = ncc_symtab_lookup(
                    st, NCC_STRING_STATIC("tag"), tag);
                if (te && te->type_node
                    && child_nt(te->type_node, "member_declaration_list")) {
                    result = te->type_node;
                }
            }
        }
    }
    if (!result) {
        ncc_sym_entry_t *te = ncc_symtab_lookup(st, NCC_STRING_STATIC("tag"),
                                                barenm);
        if (te && te->type_node
            && child_nt(te->type_node, "member_declaration_list")) {
            result = te->type_node;
        }
    }

    if (nm.data) {
        ncc_free(nm.data);
    }
    if (barenm.data) {
        ncc_free(barenm.data);
    }
    return result;
}

// Type spelling of a single member_declaration whose field name is `fname`.
static char *
member_decl_type(ncc_parse_tree_t *m, ncc_string_t fname)
{
    ncc_parse_tree_t *specs = child_nt(m, "specifier_qualifier_list");
    if (!specs) {
        return nullptr;
    }
    ncc_parse_tree_t *declr = find_descendant_nt(m, "declarator");
    int               depth = declr ? declarator_ptr_depth(declr) : 0;
    if (depth < 0) {
        return nullptr; // array / function member — not yet typed
    }
    char *base = canonical_base(specs, fname);
    return base ? append_stars(base, depth) : nullptr;
}

// Find member `name` and return its type spelling, recursing through the
// (left-recursive) member_declaration_list structure but NOT into a member's
// own nested aggregate body. Returns the type, or nullptr if not found / not
// yet typeable. `*found` is set when the name matched (even if untypeable).
static char *
find_member_type(ncc_parse_tree_t *node, ncc_string_t name, bool *found)
{
    if (!node || ncc_tree_is_leaf(node)) {
        return nullptr;
    }
    if (nt_is(node, "member_declaration")) {
        ncc_string_t fname = ncc_string_empty();
        last_field_identifier(node, &fname);
        if (fname.data && ncc_string_eq(fname, name)) {
            *found = true;
            return member_decl_type(node, fname);
        }
        return nullptr; // a different field — do not descend into its body
    }
    size_t nc = ncc_tree_num_children(node);
    for (size_t i = 0; i < nc; i++) {
        char *r = find_member_type(ncc_tree_child(node, i), name, found);
        if (*found) {
            return r;
        }
    }
    return nullptr;
}

// Type spelling of member `name` within aggregate `spec`, or nullptr.
static char *
type_of_member(ncc_parse_tree_t *spec, ncc_string_t name)
{
    ncc_parse_tree_t *members = child_nt(spec, "member_declaration_list");
    if (!members || !name.data) {
        return nullptr;
    }
    bool found = false;
    return find_member_type(members, name, &found);
}

// ============================================================================
// Expression typing
// ============================================================================

char *
ncc_type_of_expr(ncc_symtab_t *st, ncc_parse_tree_t *expr)
{
    if (!st || !expr) {
        return nullptr;
    }

    // Identifier leaf -> variable/param/function lookup.
    if (ncc_tree_is_leaf(expr)) {
        ncc_token_info_t *tok = ncc_tree_leaf_value(expr);
        if (tok && tok->tid == NCC_TOK_IDENTIFIER) {
            ncc_string_t     name = leaf_text(expr);
            ncc_sym_entry_t *sym  = ncc_symtab_lookup(st, ncc_string_empty(),
                                                      name);
            if (sym) {
                return type_of_symbol(sym);
            }
        }
        return nullptr;
    }

    // Transparent group wrappers.
    if (is_group(expr)) {
        ncc_parse_tree_t *only = nullptr;
        meaningful_children(expr, &only);
        return only ? ncc_type_of_expr(st, only) : nullptr;
    }

    // cast: ( type_name ) cast_expression  ->  the cast type.
    if (nt_is(expr, "cast_expression")) {
        ncc_parse_tree_t *tn = child_nt(expr, "type_name");
        if (tn) {
            ncc_string_t norm = ncc_normalize_type_tree(tn);
            return norm.data;
        }
    }

    // primary_expression: ( expression ) -> inner expression.
    if (nt_is(expr, "primary_expression")) {
        ncc_parse_tree_t *inner = child_nt(expr, "expression");
        if (inner) {
            return ncc_type_of_expr(st, inner);
        }
        // identifier directly under primary_expression.
        ncc_parse_tree_t *id = child_nt(expr, "identifier");
        if (id) {
            return ncc_type_of_expr(st, id);
        }
    }

    // identifier nonterminal wrapping the IDENTIFIER leaf.
    if (nt_is(expr, "identifier")) {
        ncc_parse_tree_t *only = nullptr;
        meaningful_children(expr, &only);
        if (only) {
            return ncc_type_of_expr(st, only);
        }
    }

    // unary_operator cast_expression: handle & (address-of) and * (deref).
    if (nt_is(expr, "unary_expression")) {
        ncc_parse_tree_t *op  = child_nt(expr, "unary_operator");
        ncc_parse_tree_t *rhs = child_nt(expr, "cast_expression");
        if (op && rhs) {
            // The operator token lives under unary_operator -> _ops_op_unary.
            ncc_string_t opc = ncc_string_empty();
            // Find the single operator leaf.
            ncc_parse_tree_t *opnode = op;
            while (opnode && !ncc_tree_is_leaf(opnode)) {
                ncc_parse_tree_t *only = nullptr;
                meaningful_children(opnode, &only);
                opnode = only;
            }
            opc = leaf_text(opnode);
            if (opc.data && strcmp(opc.data, "&") == 0) {
                char *inner = ncc_type_of_expr(st, rhs);
                return append_stars(inner, 1);
            }
            if (opc.data && strcmp(opc.data, "*") == 0) {
                char *inner = ncc_type_of_expr(st, rhs);
                return strip_one_star(inner);
            }
            return nullptr; // other unary operators not typed yet
        }
    }

    // postfix member access: base ('.' | '->') member.
    if (nt_is(expr, "postfix_expression")) {
        bool              arrow = false;
        bool              dot   = false;
        ncc_parse_tree_t *base  = nullptr;
        ncc_parse_tree_t *mem   = nullptr;
        size_t            nc    = ncc_tree_num_children(expr);
        for (size_t i = 0; i < nc; i++) {
            ncc_parse_tree_t *c = ncc_tree_child(expr, i);
            if (!c) {
                continue;
            }
            ncc_string_t t = leaf_text(c);
            if (t.data && strcmp(t.data, "->") == 0) {
                arrow = true;
            }
            else if (t.data && strcmp(t.data, ".") == 0) {
                dot = true;
            }
            else if (!base) {
                base = c; // first non-operator child is the aggregate operand
            }
            else {
                mem = c; // last is the member name
            }
        }
        if ((arrow || dot) && base && mem) {
            char *btype = ncc_type_of_expr(st, base);
            if (arrow) {
                btype = strip_one_star(btype); // p->m : deref then member
            }
            char *result = nullptr;
            if (btype) {
                ncc_parse_tree_t *spec = ncc_symtab_aggregate_spec(st, btype);
                if (spec) {
                    result = type_of_member(spec, identifier_text(mem));
                }
                ncc_free(btype);
            }
            return result;
        }
    }

    // Pass-through precedence cascade: a single meaningful child.
    {
        ncc_parse_tree_t *only = nullptr;
        if (meaningful_children(expr, &only) == 1 && only) {
            return ncc_type_of_expr(st, only);
        }
    }

    return nullptr; // calls, subscripts, arithmetic: TODO
}
