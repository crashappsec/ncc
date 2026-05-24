#include <stddef.h>

#define n00b_list_tid(T) typeid("n00b_list", T)
#define n00b_list_t(T)                                                               \
    _generic_struct n00b_list_tid(T) {                                               \
        T      *data;                                                                \
        size_t  len;                                                                 \
        size_t  cap;                                                                 \
        void   *lock;                                                                \
        void   *allocator;                                                           \
        int     scan_kind;                                                           \
        void   *scan_cb;                                                             \
        void   *scan_user;                                                           \
    }

n00b_list_t(int) values = l{1, 2, 3};
