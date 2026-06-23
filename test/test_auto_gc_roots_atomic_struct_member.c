typedef struct n00b_string_t n00b_string_t;

static struct {
    _Atomic(n00b_string_t *) head;
    int                     scalar;
    _Atomic(n00b_string_t *) tail[3];
} atomic_member_holder;

int
main(void)
{
    return atomic_member_holder.scalar;
}
