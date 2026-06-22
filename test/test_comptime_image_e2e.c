#include "core/alloc.h"

#include <stdint.h>

typedef struct ct_e2e_node_t {
    struct ct_e2e_node_t *next;
    uint64_t             tag;
} ct_e2e_node_t;

[[n00b::comptime]] ct_e2e_node_t *answer = 0;
[[n00b::comptime]] ct_e2e_node_t *mirror = 0;

int
comptime_main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    ct_e2e_node_t *left  = n00b_alloc(ct_e2e_node_t);
    ct_e2e_node_t *right = n00b_alloc(ct_e2e_node_t);

    left->next  = right;
    left->tag   = UINT64_C(0x4354494d47303131);
    right->next = left;
    right->tag  = UINT64_C(0x4354494d47303232);

    answer = left;
    mirror = right;

    return 0;
}

int
main(void)
{
    if (answer == 0 || mirror == 0) {
        return 10;
    }
    if (answer == mirror) {
        return 11;
    }
    if (answer->next != mirror || mirror->next != answer) {
        return 12;
    }
    if (answer->tag != UINT64_C(0x4354494d47303131)
        || mirror->tag != UINT64_C(0x4354494d47303232)) {
        return 13;
    }
    return 0;
}
