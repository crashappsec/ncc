// D-033: VLA of a pointer-bearing struct should compile under
// --ncc-gc-stack-maps and emit a single conservative whole-buffer
// scan root, sized at runtime from sizeof(arr)/sizeof(void *).

struct vla_pa_field {
    const char *name;
    const char *value;
};

void
gc_stack_maps_vla_pointer_aggregate_fixture(unsigned int n,
                                            const char *name,
                                            const char *value)
{
    struct vla_pa_field fields[n];
    for (unsigned int i = 0; i < n; i++) {
        fields[i].name  = name;
        fields[i].value = value;
    }
    (void)fields;
}
