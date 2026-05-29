// D-033 regression: the runtime-bounded VLA num_words expression for
// VLAs of pointer-bearing aggregate types must compile cleanly under
// `-Werror=sizeof-array-div`.  Clang otherwise warns when
// `sizeof(arr) / sizeof(void *)` is computed for an array whose element
// type is not `void *`; the transform must parenthesize the divisor so
// the warning is suppressed (this is what clang's own diagnostic note
// recommends).
//
// This test exists to lock in the parenthesization choice; if the
// emitter ever regresses to bare `sizeof(arr) / sizeof(void *)`, this
// test fails at compile time.

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

void
n00b_gc_stack_push(n00b_gc_stack_frame_t *frame,
                   const n00b_gc_stack_map_t *map,
                   void **roots)
{
    frame->map    = map;
    frame->roots  = roots;
    frame->active = 1;
}

void
n00b_gc_stack_pop(n00b_gc_stack_frame_t *frame)
{
    if (frame) {
        frame->active = 0;
    }
}
#pragma ncc on

struct vla_pa_field {
    const char *name;
    const char *value;
};

static void
fixture(unsigned int n, const char *name, const char *value)
{
    struct vla_pa_field fields[n];
    for (unsigned int i = 0; i < n; i++) {
        fields[i].name  = name;
        fields[i].value = value;
    }
    (void)fields;
}

int
main(void)
{
    fixture(0, "a", "b");
    return 0;
}
