typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef int n00b_gc_scan_kind_t;
typedef void (*n00b_gc_scan_cb_t)(void *, void *);

typedef _generic_struct typeid("n00b_list", int) {
    int *data;
    unsigned long len;
    unsigned long cap;
    n00b_rwlock_t *lock;
    n00b_allocator_t *allocator;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t scan_cb;
    void *scan_user;
} int_list_t;

const int_list_t numbers = l{1, 2, 3};

int
main(void)
{
    return numbers.len == 3 ? 0 : 1;
}
