#pragma ncc off
#include <stdint.h>

// Mock runtime whose struct layouts and (void *) push/pop signatures match
// ncc's type-name-free stack-map emission exactly (slot/map/frame layouts are
// mirrored by the n00b-side codegen_abi_guard.c). The frame is prev/map/roots
// (no `active`): pop is driven by the cleanup attribute, so push/pop always
// pair and simple counters suffice.
typedef struct {
    uint32_t root_index;
    uint32_t num_words;
} n00b_gc_stack_slot_t;

typedef struct {
    uint32_t    num_roots;
    uint32_t    num_slots;
    uint32_t    flags;
    const void *slots;
    const char *function_name;
    const char *file_name;
    uint32_t    line;
} n00b_gc_stack_map_t;

typedef struct n00b_gc_stack_frame_t {
    struct n00b_gc_stack_frame_t *prev;
    const n00b_gc_stack_map_t    *map;
    void                        **roots;
} n00b_gc_stack_frame_t;

static int pushed;
static int popped;
static int bad_stack_map;

void
n00b_gc_stack_push(void *frame_v, const void *map_v, void **roots)
{
    n00b_gc_stack_frame_t     *frame = frame_v;
    const n00b_gc_stack_map_t *map   = map_v;

    pushed++;
    frame->prev  = (void *)0;
    frame->map   = map;
    frame->roots = roots;

    if (!map || !roots || map->num_roots == 0
        || map->num_roots != map->num_slots || !map->slots
        || !map->function_name || !map->file_name || map->line == 0) {
        bad_stack_map = 1;
        return;
    }

    const n00b_gc_stack_slot_t *slots = map->slots;
    for (uint32_t i = 0; i < map->num_roots; i++) {
        if (!roots[i] || slots[i].root_index != i
            || slots[i].num_words == 0) {
            bad_stack_map = 1;
        }
    }
}

void
n00b_gc_stack_pop(void *frame_v)
{
    (void)frame_v;
    popped++;
}
#pragma ncc on

struct gc_stack_runtime_box {
    void *ptr;
    int   tag;
};

static int
gc_stack_maps_runtime_fixture(void *input)
{
    void *local = input;
    void *slots[2] = {local, input};
    struct gc_stack_runtime_box box = {.ptr = local, .tag = 7};

    {
        void *nested = box.ptr;
        if (nested != input) {
            return 10;
        }
    }

    return slots[0] == input ? 0 : 11;
}

int
main(void)
{
    int marker = 0;
    int rc = gc_stack_maps_runtime_fixture(&marker);

    if (rc != 0) {
        return rc;
    }
    if (bad_stack_map) {
        return 20;
    }
    if (pushed != popped) {
        return 21;
    }
    if (pushed < 5) {
        return 22;
    }

    return 0;
}
