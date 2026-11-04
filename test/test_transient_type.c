// test_transient_type.c -- [[n00b::transient]] on a whole TYPE.
//
// Marking a struct (`struct [[n00b::transient]] T`) or a typedef (prefix
// `[[n00b::transient]] typedef ...` or trailing `typedef ... name
// [[n00b::transient]]`) makes every field of that type transient:
//   * a by-value field of the type is zeroed whole;
//   * an array of it is zeroed whole;
//   * a single pointer to it is zeroed, so it marshals as null -- and it is
//     still GC-scanned (transient != noscan);
//   * a typehash of the type itself gets the trmap range [0, sizeof(T));
//   * a pointer-to-pointer and a function pointer returning T * are NOT
//     transient; unrelated siblings stay out of the table.
// This fixture is compiled and run, so the emitted trmap must be valid C.

#include <stdint.h>
#include <stddef.h>

// Struct-tag mark.
struct [[n00b::transient]] tt_handle {
    int   fd;
    void *os_ctx;
};

// Prefix typedef mark.
[[n00b::transient]] typedef struct tt_lock_t {
    uint32_t owner;
    uint32_t count;
} tt_lock_t;

// Trailing typedef mark on a scalar.
typedef int tt_fd_t [[n00b::transient]];

// Aliases inherit the mark: a plain alias and a pointer typedef.
typedef struct tt_handle  tt_handle_t;
typedef struct tt_handle *tt_handle_ref;

typedef struct tt_payload_t {
    uint64_t value;
} tt_payload_t;

typedef struct tt_holder_t {
    tt_payload_t      *keep;       // unrelated pointer: not transient
    struct tt_handle  *hptr;       // pointer to transient struct -> null
    tt_handle_t        hval;       // by-value transient (via alias)
    tt_fd_t            fd;         // transient scalar typedef
    tt_handle_ref      href;       // pointer typedef to transient
    tt_lock_t         *lptr;       // pointer to transient typedef
    tt_lock_t        **lpp;        // pointer-to-pointer: not transient
    tt_lock_t       *(*mk)(void);  // function pointer: not transient
    uint64_t           plain;      // not transient
} tt_holder_t;

// Array of a transient type, zeroed whole. Kept apart from tt_holder_t: the
// GC pointer map bails on a by-value array of aggregates, and tt_holder_t's
// map is asserted on.
typedef struct tt_array_holder_t {
    uint64_t  plain;
    tt_lock_t locks[2];
} tt_array_holder_t;

static uint64_t h_holder = typehash(tt_holder_t *);
static uint64_t h_array  = typehash(tt_array_holder_t *);
static uint64_t h_handle = typehash(tt_handle_t *);
static uint64_t h_lock   = typehash(tt_lock_t *);

int
main(void)
{
    return (int)((h_holder ^ h_array ^ h_handle ^ h_lock) == 0);
}
