#pragma once

// Type normalization for typeid/typestr/typehash transforms.
//
// Walk a parse subtree's leaf tokens, normalize the type string,
// then hash/mangle for typeid and typehash output.

#include "parse/parse_tree.h"
#include "lib/string.h"
#include <stdint.h>

// Walk a parse subtree's leaves, collect token text, normalize.
// Returns an ncc_string_t with the canonical type string.
ncc_string_t ncc_normalize_type_tree(ncc_parse_tree_t *subtree);

// Canonical type string -> SHA256 -> base64-like identifier with __ prefix.
// Returns an ncc_string_t.
ncc_string_t ncc_type_mangle(const char *normalized);

// Canonical type string -> SHA256 -> first 8 bytes as big-endian uint64.
uint64_t ncc_type_hash_u64(const char *normalized);
