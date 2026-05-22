typedef struct gc_stack_maps_inner {
    void *inner;
    int   count;
} gc_stack_maps_inner_t;

typedef struct gc_stack_maps_atomic_store {
    void *next;
    int   generation;
} gc_stack_maps_atomic_store_t;

typedef _Atomic(gc_stack_maps_atomic_store_t) gc_stack_maps_atomic_cell_t;

typedef void (*gc_stack_maps_predicate_t)(void);

struct gc_stack_maps_box {
    void                  *ptr;
    int                    tag;
    gc_stack_maps_inner_t  nested;
    void                  *ptrs[2];
};

typedef struct gc_stack_maps_filter {
    gc_stack_maps_predicate_t predicate;
    void                     *ctx;
} gc_stack_maps_filter_t;

typedef struct gc_stack_maps_flexible_tail {
    void *known;
    void *opaque[];
} gc_stack_maps_flexible_tail_t;

typedef struct gc_stack_maps_atomic_holder {
    _Atomic(gc_stack_maps_atomic_store_t *) store;
    void                                   *scan_user;
} gc_stack_maps_atomic_holder_t;

void
gc_stack_maps_roots_fixture(void *param,
                            int *items,
                            struct gc_stack_maps_box box_param)
{
    void *local = param;
    void *slots[3] = {param, items, local};
    struct gc_stack_maps_box box = {
        .ptr = local,
        .tag = 1,
        .nested = {.inner = items, .count = 2},
        .ptrs = {param, items},
    };
    gc_stack_maps_atomic_holder_t holder = {
        .store     = (gc_stack_maps_atomic_store_t *)0,
        .scan_user = local,
    };
    gc_stack_maps_filter_t filters[] = {
        {0, param},
    };
    gc_stack_maps_flexible_tail_t flex = {
        .known = param,
    };
    gc_stack_maps_atomic_cell_t atomic_store;

    {
        int *nested = items;
        (void)nested;
    }

    (void)slots;
    (void)box;
    (void)box_param;
    (void)holder;
    (void)filters;
    (void)flex;
    (void)atomic_store;
}
