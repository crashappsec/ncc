typedef struct {
    unsigned long root_index;
    unsigned long num_words;
} n00b_gc_stack_slot_t;

typedef struct {
    unsigned long               num_roots;
    unsigned long               num_slots;
    const n00b_gc_stack_slot_t *slots;
    const char                 *function_name;
    const char                 *file_name;
    unsigned int                line;
} n00b_gc_stack_map_t;

typedef struct n00b_gc_stack_frame_t {
    const n00b_gc_stack_map_t  *map;
    void                      **roots;
    struct n00b_gc_stack_frame_t *prev;
    int                         active;
} n00b_gc_stack_frame_t;

static n00b_gc_stack_frame_t *top_frame;
static int pushed;
static int popped;
static int current_depth;
static int bad_stack_map;
static void *expected;
static void *global_a;
static void *global_b;

static const n00b_gc_stack_slot_t manual_slots[] = {
    {.root_index = 0, .num_words = 1},
};

static const n00b_gc_stack_map_t manual_map = {
    .num_roots     = 1,
    .num_slots     = 1,
    .slots         = manual_slots,
    .function_name = "manual",
    .file_name     = __FILE__,
};

struct macro_option {
    void *value;
    int   has_value;
};

#define macro_wrapped_option_t(T) struct macro_option

void
n00b_gc_stack_push(n00b_gc_stack_frame_t *frame,
                   const n00b_gc_stack_map_t *map,
                   void **roots)
{
    pushed++;
    frame->map = map;
    frame->roots = roots;
    frame->prev = top_frame;
    frame->active = 1;
    top_frame = frame;
    current_depth++;

    if (!map || !roots || map->num_roots == 0
        || map->num_roots != map->num_slots || !map->slots) {
        bad_stack_map = 1;
    }
}

void
n00b_gc_stack_pop(n00b_gc_stack_frame_t *frame)
{
    popped++;
    if (!frame || !frame->active || top_frame != frame) {
        bad_stack_map = 1;
        return;
    }

    top_frame = frame->prev;
    frame->active = 0;
    current_depth--;
}

int
n00b_gc_stack_test_depth(void)
{
    return current_depth;
}

int
n00b_gc_stack_test_contains_expected(void)
{
    for (n00b_gc_stack_frame_t *frame = top_frame; frame; frame = frame->prev) {
        for (unsigned long i = 0; i < frame->map->num_slots; i++) {
            const n00b_gc_stack_slot_t *slot = &frame->map->slots[i];
            void **words = (void **)frame->roots[slot->root_index];
            for (unsigned long w = 0; w < slot->num_words; w++) {
                if (words[w] == expected) {
                    return 1;
                }
            }
        }
    }

    return 0;
}

static int
gc_stack_maps_mutation_fixture(void)
{
    void *local = global_a;

    expected = global_a;
    if (!n00b_gc_stack_test_contains_expected()) {
        return 10;
    }

    local = global_b;
    expected = global_b;
    if (!n00b_gc_stack_test_contains_expected()) {
        return 11;
    }

    expected = global_a;
    if (n00b_gc_stack_test_contains_expected()) {
        return 12;
    }

    return local == global_b ? 0 : 13;
}

static int
gc_stack_maps_early_return_fixture(void *input)
{
    void *outer = input;

    {
        void *inner = outer;
        expected = input;
        if (!n00b_gc_stack_test_contains_expected()) {
            return 20;
        }
        return inner == input ? 0 : 21;
    }
}

static int
gc_stack_maps_break_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    while (1) {
        void *inner = input;
        expected = input;
        if (!inner || !n00b_gc_stack_test_contains_expected()) {
            return 30;
        }
        break;
    }

    return n00b_gc_stack_test_depth() == base ? 0 : 31;
}

static int
gc_stack_maps_continue_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    for (int i = 0; i < 2; i++) {
        void *inner = input;
        expected = input;
        if (!inner || !n00b_gc_stack_test_contains_expected()) {
            return 40;
        }
        continue;
    }

    return n00b_gc_stack_test_depth() == base ? 0 : 41;
}

static int
gc_stack_maps_switch_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    switch (1) {
    case 1: {
        void *inner = input;
        expected = input;
        if (!inner || !n00b_gc_stack_test_contains_expected()) {
            return 50;
        }
        break;
    }
    default:
        return 51;
    }

    return n00b_gc_stack_test_depth() == base ? 0 : 52;
}

static int
gc_stack_maps_goto_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    {
        void *inner = input;
        expected = input;
        if (!inner || !n00b_gc_stack_test_contains_expected()) {
            return 60;
        }
        goto out;
    }

    return 61;

out:
    return n00b_gc_stack_test_depth() == base ? 0 : 62;
}

static int
gc_stack_maps_forward_goto_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    goto add_arg;

    void *skipped = input;
    if (skipped) {
        return 70;
    }

    macro_wrapped_option_t(unsigned long) macro_skipped =
        (struct macro_option){.value = input, .has_value = input != 0};
    if (macro_skipped.has_value) {
        return 73;
    }

add_arg:;
    {
        void *after = input;
        expected = input;
        if (!after || !n00b_gc_stack_test_contains_expected()) {
            return 71;
        }
    }

    return n00b_gc_stack_test_depth() == base ? 0 : 72;
}

static int
gc_stack_maps_manual_api_fixture(void *input)
{
    int base = n00b_gc_stack_test_depth();

    void                 *local = input;
    void                 *roots[] = {&local};
    n00b_gc_stack_frame_t frame;

    n00b_gc_stack_push(&frame, &manual_map, roots);
    expected = input;
    if (!local || !n00b_gc_stack_test_contains_expected()) {
        return 80;
    }
    n00b_gc_stack_pop(&frame);

    return n00b_gc_stack_test_depth() == base ? 0 : 81;
}

static int
check_result(int rc)
{
    if (rc != 0) {
        return rc;
    }
    if (bad_stack_map) {
        return 90;
    }
    if (n00b_gc_stack_test_depth() != 0) {
        return 91;
    }
    if (pushed != popped) {
        return 92;
    }
    return 0;
}

int
main(void)
{
    int a = 1;
    int b = 2;
    int rc = 0;

    global_a = &a;
    global_b = &b;

    rc = check_result(gc_stack_maps_mutation_fixture());
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_early_return_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_break_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_continue_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_switch_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_goto_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_forward_goto_fixture(&a));
    if (rc) {
        return rc;
    }
    rc = check_result(gc_stack_maps_manual_api_fixture(&a));
    if (rc) {
        return rc;
    }

    return 0;
}
