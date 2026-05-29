// D-033: VLA of an aggregate type that contains NO pointer fields
// should NOT trigger the runtime-bounded aggregate-VLA path -- it
// produces zero roots (the array contributes nothing to scan)
// and must compile cleanly under --ncc-gc-stack-maps.

struct vla_sa_pair {
    unsigned int id;
    unsigned int tag;
};

void
gc_stack_maps_vla_scalar_aggregate_fixture(unsigned int n)
{
    struct vla_sa_pair pairs[n];
    for (unsigned int i = 0; i < n; i++) {
        pairs[i].id  = i;
        pairs[i].tag = i * 2u;
    }
    (void)pairs;
}
