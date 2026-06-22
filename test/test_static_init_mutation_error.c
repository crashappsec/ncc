#include "core/alloc.h"

#include <stdint.h>

typedef struct static_init_mut_node_t {
    uint64_t tag;
} static_init_mut_node_t;

static static_init_mut_node_t *
make_static_state(void)
{
    static_init_mut_node_t *node = n00b_alloc(static_init_mut_node_t);
    node->tag = UINT64_C(0x4d55544154453031);
    return node;
}

const static_init_mut_node_t *state = make_static_state();

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    ((static_init_mut_node_t *)state)->tag = UINT64_C(0x4d55544154453032);
    return 0;
}

int
main(void)
{
    return 0;
}
