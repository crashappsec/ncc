// WP-011 Phase 2 fixture: positive grammar recognition for dict
// literals.  All forms here must parse cleanly under the C_NCC.bnf
// extensions (`<dict_literal>` and the `d{...}` <modified_literal>
// modifier).  Phase 2 wires this through `expect_error_contains` for
// the stub-lowering diagnostic ("dict lowering not yet implemented in
// WP-011 Phase 2"): the parser accepting every dict-literal form
// reaches the xform pass, which then trips the stub on the first
// declaration.  Once Phase 3 implements the helper-backed dict
// lowering, this fixture flips to `compile_run` with the n00b list
// literal assertion shape (typedefs, module-scope decls, `main()`
// running assertions) — that is why the file is structured this way
// today.
//
// Phase 3 will:
//   * delete the `expect_error_contains` mode wiring in meson.build,
//   * delete `err_dict_literal_stub.c`,
//   * uncomment the `main()` assertions below and flip this fixture
//     to `compile_run`.
//
// The Phase 2 test only exercises parser recognition; it does not
// require the runtime layout to be sound.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ncc_runtime.h"

typedef struct n00b_rwlock_t {
    int placeholder;
} n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef struct n00b_gc_map_t n00b_gc_map_t;
typedef void (*n00b_gc_scan_cb_t)(n00b_gc_map_t *, void *);

typedef enum n00b_gc_scan_kind_t : uint8_t {
    N00B_GC_SCAN_KIND_DEFAULT  = 0,
    N00B_GC_SCAN_KIND_NONE     = 1,
    N00B_GC_SCAN_KIND_ALL      = 2,
    N00B_GC_SCAN_KIND_CALLBACK = 4,
} n00b_gc_scan_kind_t;

#define n00b_dict_tid(K, V) typeid("n00b_dict", K, V)
#define n00b_dict_t(K, V)                                                     \
    _generic_struct n00b_dict_tid(K, V) {                                     \
        K                  *keys;                                             \
        V                  *values;                                           \
        size_t              len;                                              \
        size_t              cap;                                              \
        n00b_rwlock_t      *lock;                                             \
        n00b_allocator_t   *allocator;                                        \
        n00b_gc_scan_kind_t scan_kind;                                        \
        n00b_gc_scan_cb_t   scan_cb;                                          \
        void               *scan_user;                                        \
    }

#define n00b_list_tid(T) typeid("n00b_list", T)
#define n00b_list_t(T)                                                        \
    _generic_struct n00b_list_tid(T) {                                        \
        T                  *data;                                             \
        size_t              len;                                              \
        size_t              cap;                                              \
        n00b_rwlock_t      *lock;                                             \
        n00b_allocator_t   *allocator;                                        \
        n00b_gc_scan_kind_t scan_kind;                                        \
        n00b_gc_scan_cb_t   scan_cb;                                          \
        void               *scan_user;                                        \
    }

// Explicit `d{...}` form, scalar K and V.
n00b_dict_t(int, int) module_kv_explicit = d{1: 2};

// Explicit empty `d{}` form (D-064 requires this to be a valid dict
// literal at the parse level).
n00b_dict_t(int, int) module_kv_empty = d{};

// Bare `{key: value}` form — disambiguated from C compound init by
// the presence of `:` separators (D-063).
n00b_dict_t(int, int) module_kv_bare = {1: 2};

// Multi-pair explicit form.
n00b_dict_t(int, int) module_kv_multi = d{1: 2, 3: 4, 5: 6};

// Nested supported container as a dict value (D-069: nested
// arrays/lists/dicts allowed as values).
n00b_dict_t(int, n00b_list_t(int)) module_kv_nested = d{1: l{2, 3}};

int
main(void)
{
    // Phase 3 will activate these assertions once the helper-backed
    // lowering replaces the stub.  Until then, the xform pass aborts
    // on the first module-scope dict literal with the
    // "dict lowering not yet implemented in WP-011 Phase 2"
    // diagnostic, and `main()` is never reached.
    (void)module_kv_explicit;
    (void)module_kv_empty;
    (void)module_kv_bare;
    (void)module_kv_multi;
    (void)module_kv_nested;
    return 0;
}
