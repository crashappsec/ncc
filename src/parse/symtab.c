// symtab.c - Symbol table with named parallel namespaces.
//
// Each namespace has its own scope stack and hash table. Push/pop
// scope is per-namespace. Symbol lookup returns the innermost
// (most recent) definition. Shadowed entries are restored on pop.

#include "parse/symtab.h"
#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/string.h"

#include <string.h>

// ============================================================================
// Hash table helpers (thin wrappers around ncc_dict_t)
// ============================================================================

static ncc_dict_t *
ht_new(void)
{
    ncc_dict_t *d = ncc_alloc(ncc_dict_t);
    ncc_dict_init(d, ncc_hash_cstring, ncc_dict_cstr_eq);
    return d;
}

static void
ht_put(ncc_dict_t *d, ncc_string_t key, ncc_sym_entry_t *val)
{
    ncc_dict_put(d, key.data, val);
}

static ncc_sym_entry_t *
ht_get(ncc_dict_t *d, ncc_string_t key)
{
    if (!d || !key.data) {
        return nullptr;
    }

    bool found = false;
    void *val  = ncc_dict_get(d, key.data, &found);

    return found ? (ncc_sym_entry_t *)val : nullptr;
}

static void
ht_remove(ncc_dict_t *d, ncc_string_t key)
{
    if (d && key.data) {
        ncc_dict_remove(d, key.data);
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

ncc_symtab_t *
ncc_symtab_new(void)
{
    ncc_symtab_t *st = ncc_alloc(ncc_symtab_t);
    return st;
}

void
ncc_symtab_free(ncc_symtab_t *st)
{
    if (!st) {
        return;
    }

    // Scopes are retained (pop no longer frees), so free every scope and its
    // entries via the retained list. Each entry belongs to exactly one scope's
    // chain, so this frees each exactly once.
    ncc_scope_t *scope = st->all_scopes;
    while (scope) {
        ncc_scope_t     *next_scope = scope->all_next;
        ncc_sym_entry_t *entry      = scope->first_in_scope;
        while (entry) {
            ncc_sym_entry_t *next = entry->next_in_scope;
            ncc_free(entry);
            entry = next;
        }
        ncc_free(scope);
        scope = next_scope;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        ncc_namespace_t *ns = &st->namespaces[i];
        if (ns->symbols) {
            ncc_dict_free((ncc_dict_t *)ns->symbols);
            ncc_free(ns->symbols);
        }
    }

    if (st->namespaces) {
        ncc_free(st->namespaces);
    }

    ncc_free(st);
}

// ============================================================================
// Namespace management
// ============================================================================

ncc_namespace_t *
ncc_symtab_ns(ncc_symtab_t *st, ncc_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (ncc_string_eq(st->namespaces[i].ns_name, ns_name)) {
            return &st->namespaces[i];
        }
    }

    if (st->ns_count >= st->ns_cap) {
        int32_t new_cap = st->ns_cap ? st->ns_cap * 2 : 4;
        ncc_namespace_t *new_ns = ncc_alloc_array(ncc_namespace_t, new_cap);

        if (st->ns_count > 0) {
            memcpy(new_ns, st->namespaces,
                   (size_t)st->ns_count * sizeof(ncc_namespace_t));
        }

        if (st->namespaces) {
            ncc_free(st->namespaces);
        }

        st->namespaces = new_ns;
        st->ns_cap     = new_cap;
    }

    ncc_namespace_t *ns = &st->namespaces[st->ns_count++];
    memset(ns, 0, sizeof(*ns));
    ns->ns_name = ns_name;
    ns->symbols = ht_new();

    return ns;
}

// ============================================================================
// Scope management
// ============================================================================

void
ncc_symtab_push_scope(ncc_symtab_t *st, ncc_string_t ns_name,
                        ncc_string_t scope_name)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (!ns) {
        return;
    }

    ncc_scope_t *scope = ncc_alloc(ncc_scope_t);
    scope->parent = ns->current;
    scope->name   = scope_name;
    scope->depth  = ns->depth;

    // Retain every pushed scope for the symtab's lifetime so the lexical scope
    // tree survives population (pop no longer frees) and location-based scoped
    // lookup can resolve locals/params after the walk.
    scope->all_next = st->all_scopes;
    st->all_scopes  = scope;

    ns->current = scope;
    ns->depth++;
}

void
ncc_symtab_pop_scope(ncc_symtab_t *st, ncc_string_t ns_name)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (!ns || !ns->current) {
        return;
    }

    ncc_scope_t        *scope = ns->current;
    ncc_dict_t *ht    = (ncc_dict_t *)ns->symbols;

    // Restore the hash table to the enclosing scope's view (so lookups during
    // the rest of the walk stay correct), but DO NOT free the scope or its
    // entries — they are retained (via all_scopes / first_in_scope chains) for
    // post-walk scoped lookup and freed only in ncc_symtab_free.
    ncc_sym_entry_t *entry = scope->first_in_scope;

    while (entry) {
        if (entry->shadowed) {
            ht_put(ht, entry->name, entry->shadowed);
        }
        else {
            ht_remove(ht, entry->name);
        }
        entry = entry->next_in_scope;
    }

    ns->current = scope->parent;
    ns->depth--;
}

// ============================================================================
// Symbol operations
// ============================================================================

ncc_sym_entry_t *
ncc_symtab_add(ncc_symtab_t *st, ncc_string_t ns_name,
                 ncc_string_t name, ncc_sym_kind_t kind,
                 ncc_parse_tree_t *decl_node)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);

    if (!ns) {
        return nullptr;
    }

    ncc_dict_t *ht = (ncc_dict_t *)ns->symbols;

    ncc_sym_entry_t *entry = ncc_alloc(ncc_sym_entry_t);
    entry->name        = ncc_string_from_raw(name.data, (int64_t)name.u8_bytes);
    entry->kind        = kind;
    entry->scope_depth = ns->depth;
    entry->decl_node   = decl_node;

    ncc_sym_entry_t *existing = ht_get(ht, name);

    if (existing) {
        entry->shadowed = existing;
    }

    ht_put(ht, name, entry);

    if (ns->current) {
        entry->next_in_scope         = ns->current->first_in_scope;
        ns->current->first_in_scope = entry;
    }

    return entry;
}

ncc_sym_entry_t *
ncc_symtab_lookup(ncc_symtab_t *st, ncc_string_t ns_name,
                    ncc_string_t name)
{
    if (!st) {
        return nullptr;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (ncc_string_eq(st->namespaces[i].ns_name, ns_name)) {
            return ht_get((ncc_dict_t *)st->namespaces[i].symbols,
                          name);
        }
    }

    return nullptr;
}

void
ncc_symtab_set_scope_node(ncc_symtab_t *st, ncc_string_t ns_name,
                          ncc_parse_tree_t *node)
{
    ncc_namespace_t *ns = ncc_symtab_ns(st, ns_name);
    if (ns && ns->current) {
        ns->current->node = node;
    }
}

ncc_scope_t *
ncc_symtab_scope_for_node(ncc_symtab_t *st, ncc_parse_tree_t *node)
{
    if (!st || !node) {
        return nullptr;
    }
    for (ncc_scope_t *s = st->all_scopes; s; s = s->all_next) {
        if (s->node == node) {
            return s;
        }
    }
    return nullptr;
}

ncc_sym_entry_t *
ncc_symtab_lookup_scoped(ncc_symtab_t *st, ncc_scope_t *scope,
                         ncc_string_t ns_name, ncc_string_t name)
{
    // Walk the lexical scope chain (innermost first). Globals live in the
    // retained file-scope scope at the top of the chain, so this covers locals,
    // parameters, and globals with correct shadowing.
    for (ncc_scope_t *s = scope; s; s = s->parent) {
        for (ncc_sym_entry_t *e = s->first_in_scope; e; e = e->next_in_scope) {
            if (ncc_string_eq(e->name, name)) {
                return e;
            }
        }
    }
    // No enclosing scope (or not found): fall back to a plain global lookup.
    return ncc_symtab_lookup(st, ns_name, name);
}

bool
ncc_symtab_is_typedef(ncc_symtab_t *st, ncc_string_t name)
{
    ncc_sym_entry_t *entry = ncc_symtab_lookup(st, ncc_string_empty(), name);

    return entry && entry->kind == NCC_SYM_TYPEDEF;
}

int32_t
ncc_symtab_depth(ncc_symtab_t *st, ncc_string_t ns_name)
{
    if (!st) {
        return -1;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (ncc_string_eq(st->namespaces[i].ns_name, ns_name)) {
            return st->namespaces[i].depth;
        }
    }

    return -1;
}

ncc_scope_t *
ncc_symtab_current_scope(ncc_symtab_t *st, ncc_string_t ns_name)
{
    if (!st) {
        return nullptr;
    }

    for (int32_t i = 0; i < st->ns_count; i++) {
        if (ncc_string_eq(st->namespaces[i].ns_name, ns_name)) {
            return st->namespaces[i].current;
        }
    }

    return nullptr;
}
