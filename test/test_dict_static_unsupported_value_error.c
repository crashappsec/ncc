typedef struct n00b_rwlock_t n00b_rwlock_t;
typedef struct n00b_allocator_t n00b_allocator_t;
typedef int n00b_gc_scan_kind_t;
typedef void (*n00b_gc_scan_cb_t)(void *, void *);

typedef struct n00b_dict_bucket_t n00b_dict_bucket_t;
typedef struct __n00b_internal_type_erased_store_t {
    unsigned int        last_slot;
    unsigned int        threshold;
    unsigned int        used_count;
    n00b_dict_bucket_t *buckets;
    void              **keys;
    void              **values;
} __n00b_internal_type_erased_store_t;

#define n00b_dict_tid(K, V) typeid("n00b_dict", K, V)
#define n00b_dict_t(K, V)                                                     \
    _generic_struct n00b_dict_tid(K, V) {                                     \
        __n00b_internal_type_erased_store_t *store;                           \
        void                *fn;                                              \
        n00b_allocator_t    *allocator;                                       \
        int                  insertion_epoch;                                 \
        int                  wait_ct;                                         \
        int                  length;                                          \
        int                  _migration_state;                                \
        n00b_rwlock_t       *lock;                                            \
        unsigned char        cache;                                           \
        unsigned char        skip_obj_hash;                                   \
        n00b_gc_scan_kind_t  scan_kind;                                       \
        n00b_gc_scan_cb_t    scan_cb;                                         \
        void                *scan_user;                                       \
        n00b_gc_scan_kind_t  key_scan_kind;                                   \
        n00b_gc_scan_kind_t  value_scan_kind;                                 \
        unsigned long long   key_tid;                                         \
        unsigned long long   value_tid;                                       \
    }

typedef n00b_dict_t(unsigned long long, unsigned long long) inner_dict_t;

n00b_dict_t(unsigned long long, inner_dict_t) dict = d{1: d{2: 3}};

int
main(void)
{
    return dict.length == 1 ? 0 : 1;
}
