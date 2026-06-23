#include "core/alloc.h"
#include "core/gc.h"
#include "core/runtime.h"

#include <stdint.h>

typedef struct static_init_writable_node_t {
    struct static_init_writable_node_t *next;
    uint64_t                           tag;
} static_init_writable_node_t;

static static_init_writable_node_t *
make_static_state(void)
{
    static_init_writable_node_t *left  = n00b_alloc(static_init_writable_node_t);
    static_init_writable_node_t *right = n00b_alloc(static_init_writable_node_t);

    left->next  = right;
    left->tag   = UINT64_C(0x5752495445303031);
    right->next = left;
    right->tag  = UINT64_C(0x5752495445303032);

    return left;
}

static_init_writable_node_t *state = make_static_state();

int
main(void)
{
    if (state == nullptr || state->next == nullptr) {
        return 10;
    }
    if (state->tag != UINT64_C(0x5752495445303031)) {
        return 11;
    }
    if (state->next->tag != UINT64_C(0x5752495445303032)) {
        return 12;
    }

    state->tag = UINT64_C(0x57524954454d5554);
    if (state->tag != UINT64_C(0x57524954454d5554)) {
        return 13;
    }

    static_init_writable_node_t *heap = n00b_alloc(static_init_writable_node_t);
    heap->next = state;
    heap->tag  = UINT64_C(0x5752495445474331);
    state->next = heap;
    heap = nullptr;

    n00b_collect(n00b_get_runtime()->default_arena);

    if (state == nullptr || state->next == nullptr) {
        return 14;
    }
    if (state->tag != UINT64_C(0x57524954454d5554)) {
        return 15;
    }
    if (state->next->tag != UINT64_C(0x5752495445474331)) {
        return 16;
    }
    if (state->next->next != state) {
        return 17;
    }
    return 0;
}
