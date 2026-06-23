typedef struct n00b_string_t n00b_string_t;
typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef int n00b_gc_scan_kind_t;
typedef void (*n00b_gc_scan_cb_t)(void *, void *);

typedef _generic_struct typeid("n00b_list", n00b_string_t *) {
    n00b_string_t **data;
    unsigned long len;
    unsigned long cap;
    n00b_rwlock_t *lock;
    n00b_allocator_t *allocator;
    n00b_gc_scan_kind_t scan_kind;
    n00b_gc_scan_cb_t scan_cb;
    void *scan_user;
} string_list_t;

string_list_t words = l{r"[|b|]alpha[|/b|]"};

int
main(void)
{
    return words.len == 1 ? 0 : 1;
}
