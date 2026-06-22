#include "core/alloc.h"

#include <stdint.h>

typedef struct static_init_node_t {
    struct static_init_node_t *next;
    uint64_t                  tag;
} static_init_node_t;

int prepare_count = 0;

static static_init_node_t *
make_static_state(void)
{
    prepare_count++;

    static_init_node_t *left  = n00b_alloc(static_init_node_t);
    static_init_node_t *right = n00b_alloc(static_init_node_t);

    left->next  = right;
    left->tag   = UINT64_C(0x5354415449433031);
    right->next = left;
    right->tag  = UINT64_C(0x5354415449433032);

    return left;
}

const static_init_node_t *state = make_static_state();

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    return prepare_count == 1 ? 0 : 90;
}

int
main(void)
{
    if (state == nullptr || state->next == nullptr) {
        return 10;
    }
    if (state->tag != UINT64_C(0x5354415449433031)) {
        return 11;
    }
    if (state->next->tag != UINT64_C(0x5354415449433032)) {
        return 12;
    }
    if (state->next->next != state) {
        return 13;
    }
    return 0;
}
