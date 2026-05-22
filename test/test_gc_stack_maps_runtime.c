#pragma ncc off
typedef struct {
    unsigned long root_index;
    unsigned long num_words;
} n00b_gc_stack_slot_t;

typedef struct {
    unsigned long                 num_roots;
    unsigned long                 num_slots;
    const n00b_gc_stack_slot_t   *slots;
    const char                   *function_name;
    const char                   *file_name;
    unsigned int                  line;
} n00b_gc_stack_map_t;

typedef struct {
    const n00b_gc_stack_map_t *map;
    void                    **roots;
    int                       active;
} n00b_gc_stack_frame_t;

static int pushed;
static int popped;
static int bad_stack_map;

void
n00b_gc_stack_push(n00b_gc_stack_frame_t *frame,
                   const n00b_gc_stack_map_t *map,
                   void **roots)
{
    pushed++;
    frame->map = map;
    frame->roots = roots;
    frame->active = 1;

    if (!map || !roots || map->num_roots == 0
        || map->num_roots != map->num_slots || !map->slots
        || !map->function_name || !map->file_name || map->line == 0) {
        bad_stack_map = 1;
        return;
    }

    for (unsigned long i = 0; i < map->num_roots; i++) {
        if (!roots[i] || map->slots[i].root_index != i
            || map->slots[i].num_words == 0) {
            bad_stack_map = 1;
        }
    }
}

void
n00b_gc_stack_pop(n00b_gc_stack_frame_t *frame)
{
    popped++;
    if (!frame || !frame->active) {
        bad_stack_map = 1;
        return;
    }
    frame->active = 0;
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
