typedef struct n00b_string_t n00b_string_t;

static struct {
    _Atomic(n00b_string_t *) head;
    int                     scalar;
    _Atomic(n00b_string_t *) tail[3];
    int                     scalar2; // keeps `label` out of tail's run
    // A builtin pointee: the parser reads `label` as a typedef name (ncc#67).
    _Atomic(const char *)    label;
} atomic_member_holder;

int
main(void)
{
    return atomic_member_holder.scalar;
}
