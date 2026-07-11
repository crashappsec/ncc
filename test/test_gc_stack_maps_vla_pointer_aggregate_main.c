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
#include <stdint.h>

// Minimal mock matching ncc's type-name-free stack-map emission: (void *)
// push/pop signatures and prev/map/roots frame layout. This test only checks
// that the emitted VLA num_words expression compiles cleanly, so the bodies
// are no-ops.
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

void
n00b_gc_stack_push(void *frame, const void *map, void **roots)
{
    (void)frame;
    (void)map;
    (void)roots;
}

void
n00b_gc_stack_pop(void *frame)
{
    (void)frame;
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
