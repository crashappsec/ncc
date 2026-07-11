#include <stdint.h>

// Mock runtime whose struct layouts and (void *) push/pop signatures match
// ncc's type-name-free stack-map emission exactly (mirrored by the n00b-side
// codegen_abi_guard.c). The frame is prev/map/roots (no `active`); push/pop
// maintain the top_frame chain via `prev`.
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

struct gc_stack_maps_exact_inner {
    void *ptr;
    int   scalar;
};

struct gc_stack_maps_exact_outer {
    unsigned long                    disguised;
    struct gc_stack_maps_exact_inner inner;
    void                            *ptrs[2];
    int                              tag;
};

void
n00b_gc_stack_push(void *frame_v, const void *map_v, void **roots)
{
    n00b_gc_stack_frame_t     *frame = frame_v;
    const n00b_gc_stack_map_t *map   = map_v;

    pushed++;
    frame->prev  = top_frame;
    frame->map   = map;
    frame->roots = roots;
    top_frame    = frame;
    current_depth++;

    if (!map || !roots || map->num_roots == 0
        || map->num_roots != map->num_slots || !map->slots) {
        bad_stack_map = 1;
    }
}

void
n00b_gc_stack_pop(void *frame_v)
{
    n00b_gc_stack_frame_t *frame = frame_v;

    popped++;
    if (!frame || top_frame != frame) {
        bad_stack_map = 1;
        return;
    }

    top_frame = frame->prev;
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
        const n00b_gc_stack_slot_t *slots = frame->map->slots;
        for (uint32_t i = 0; i < frame->map->num_slots; i++) {
            const n00b_gc_stack_slot_t *slot = &slots[i];
            void **words = (void **)frame->roots[slot->root_index];
            for (uint32_t w = 0; w < slot->num_words; w++) {
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
gc_stack_maps_aggregate_precision_fixture(void *live, unsigned long dead_bits)
{
    struct gc_stack_maps_exact_outer agg = {
        .disguised = dead_bits,
        .inner = {.ptr = live, .scalar = 1},
        .ptrs = {live, 0},
        .tag = 7,
    };

    expected = live;
    if (!n00b_gc_stack_test_contains_expected()) {
        return 100;
    }

    expected = (void *)dead_bits;
    if (n00b_gc_stack_test_contains_expected()) {
        return 101;
    }

    return agg.inner.ptr == live ? 0 : 102;
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
    rc = check_result(gc_stack_maps_aggregate_precision_fixture(&a,
                                                               (unsigned long)&b));
    if (rc) {
        return rc;
    }

    return 0;
}
