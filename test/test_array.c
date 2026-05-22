#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/array.h"
#include "lib/list.h"

typedef char *ncc_test_char_ptr_t;

ncc_array_decl(int);
ncc_array_decl(ncc_test_char_ptr_t);

_Static_assert(offsetof(ncc_array_t(int), data)
               < offsetof(ncc_array_t(int), len));
_Static_assert(offsetof(ncc_array_t(int), len)
               < offsetof(ncc_array_t(int), cap));
_Static_assert(offsetof(ncc_array_t(int), cap)
               < offsetof(ncc_array_t(int), lock));
_Static_assert(offsetof(ncc_array_t(int), lock)
               < offsetof(ncc_array_t(int), allocator));
_Static_assert(offsetof(ncc_array_t(int), allocator)
               < offsetof(ncc_array_t(int), scan_kind));
_Static_assert(offsetof(ncc_array_t(int), scan_kind)
               < offsetof(ncc_array_t(int), scan_cb));
_Static_assert(offsetof(ncc_array_t(int), scan_cb)
               < offsetof(ncc_array_t(int), scan_user));
_Static_assert(sizeof(((ncc_array_t(int) *)0)->scan_kind)
               == sizeof(uint8_t));

static void
test_scan_cb(ncc_gc_map_t *map, void *user)
{
    (void)map;
    (void)user;
}

static void
test_designated_initializer_shape(void)
{
    int data[] = {1, 2, 3};
    int allocator_token;
    int scan_user;

    ncc_array_t(int) xs = {
        .data      = data,
        .len       = 3,
        .cap       = 3,
        .lock      = nullptr,
        .allocator = (ncc_allocator_t *)&allocator_token,
        .scan_kind = NCC_GC_SCAN_KIND_ALL,
        .scan_cb   = test_scan_cb,
        .scan_user = &scan_user,
    };

    assert(xs.data == data);
    assert(xs.len == 3);
    assert(xs.cap == 3);
    assert(xs.lock == nullptr);
    assert(xs.allocator == (ncc_allocator_t *)&allocator_token);
    assert(xs.scan_kind == NCC_GC_SCAN_KIND_ALL);
    assert(xs.scan_cb == test_scan_cb);
    assert(xs.scan_user == &scan_user);
}

static void
test_pointer_element_designated_initializer(void)
{
    char               a[]     = "a";
    char               b[]     = "b";
    ncc_test_char_ptr_t data[] = {a, b};

    ncc_array_t(ncc_test_char_ptr_t) xs = {
        .data      = data,
        .len       = 2,
        .cap       = 2,
        .lock      = nullptr,
        .allocator = nullptr,
        .scan_kind = NCC_GC_SCAN_KIND_ALL,
        .scan_cb   = nullptr,
        .scan_user = nullptr,
    };

    assert(xs.data[0] == a);
    assert(xs.data[1] == b);
    assert(xs.len == 2);
    assert(xs.cap == 2);
    assert(xs.scan_kind == NCC_GC_SCAN_KIND_ALL);
}

static void
test_new_checked_ptr_and_clone(void)
{
    ncc_array_t(int) xs = ncc_array_new(int, 4);

    assert(xs.data != nullptr);
    assert(xs.len == 0);
    assert(xs.cap == 4);
    assert(xs.lock == nullptr);
    assert(xs.allocator == nullptr);
    assert(xs.scan_kind == NCC_GC_SCAN_KIND_DEFAULT);
    assert(xs.scan_cb == nullptr);
    assert(xs.scan_user == nullptr);

    ncc_array_set(xs, 0, 10);
    ncc_array_set(xs, 2, 30);
    assert(xs.len == 3);
    assert(ncc_array_get(xs, 0) == 10);
    assert(ncc_array_get(xs, 2) == 30);

    int scan_user;
    xs.scan_kind = NCC_GC_SCAN_KIND_CALLBACK;
    xs.scan_cb   = test_scan_cb;
    xs.scan_user = &scan_user;

    ncc_array_t(int) copy = ncc_array_clone(xs);
    assert(copy.data != xs.data);
    assert(copy.len == xs.len);
    assert(copy.cap == xs.cap);
    assert(copy.lock == nullptr);
    assert(copy.allocator == nullptr);
    assert(copy.scan_kind == NCC_GC_SCAN_KIND_CALLBACK);
    assert(copy.scan_cb == test_scan_cb);
    assert(copy.scan_user == &scan_user);
    assert(ncc_array_get(copy, 0) == 10);
    assert(ncc_array_get(copy, 2) == 30);

    ncc_array_free(copy);
    ncc_array_free(xs);

    int backing[2]          = {0, 0};
    ncc_array_t(int) view   = ncc_array_checked_ptr(int, 2, backing);
    ncc_array_set(view, 1, 42);
    assert(view.data == backing);
    assert(view.len == 2);
    assert(view.cap == 2);
    assert(view.lock == nullptr);
    assert(view.allocator == nullptr);
    assert(view.scan_kind == NCC_GC_SCAN_KIND_DEFAULT);
    assert(view.scan_cb == nullptr);
    assert(view.scan_user == nullptr);
    assert(backing[1] == 42);
}

static void
test_list_to_array_sets_compat_fields(void)
{
    ncc_list_t(int) list = ncc_list_new(int);
    ncc_list_push(list, 11);
    ncc_list_push(list, 22);

    ncc_array_t(int) xs = ncc_list_to_array(int, list);
    assert(xs.len == 2);
    assert(xs.cap >= 2);
    assert(xs.lock == nullptr);
    assert(xs.allocator == nullptr);
    assert(xs.scan_kind == NCC_GC_SCAN_KIND_DEFAULT);
    assert(xs.scan_cb == nullptr);
    assert(xs.scan_user == nullptr);
    assert(ncc_array_get(xs, 0) == 11);
    assert(ncc_array_get(xs, 1) == 22);

    assert(list.data == nullptr);
    assert(list.len == 0);
    assert(list.cap == 0);

    ncc_array_free(xs);
}

int
main(void)
{
    test_designated_initializer_shape();
    test_pointer_element_designated_initializer();
    test_new_checked_ptr_and_clone();
    test_list_to_array_sets_compat_fields();

    return 0;
}
