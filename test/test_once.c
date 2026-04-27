#include <stdio.h>
#include <threads.h>

static int void_hits = 0;
static int value_hits = 0;
static int param_hits = 0;
static int param_first_value = 0;
static int param_first_scale = 0;
static int concurrent_hits = 0;
static int concurrent_ready = 0;
static int concurrent_release = 0;
static int pointer_value = 41;
static int pointer_hits = 0;

#define CONCURRENT_THREAD_COUNT 8

_Once void
init_once(void)
{
    void_hits++;
}

_Once int
get_once(void)
{
    value_hits++;
    return value_hits * 10;
}

_Once int
get_param_once(int value, int scale)
{
    param_hits++;
    param_first_value = value;
    param_first_scale = scale;
    return value * scale;
}

_Once int
(__attribute__((unused)) decorated_once)(int value)
{
    return value + 1;
}

_Once int
param_attr_once(int (__attribute__((unused)) value), int scale)
{
    return value * scale;
}

static long _Once
interleaved_once(void)
{
    return 1099511627776L;
}

_Once int *
get_ptr_once(void)
{
    pointer_hits++;
    return &pointer_value;
}

_Once int
get_concurrent_once(int value)
{
    __atomic_fetch_add(&concurrent_hits, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 10000; i++) {
        thrd_yield();
    }
    return value + 1000;
}

typedef struct {
    int result;
} concurrent_arg_t;

static int
call_concurrent_once(void *arg)
{
    concurrent_arg_t *thread_arg = arg;

    __atomic_add_fetch(&concurrent_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&concurrent_release, __ATOMIC_ACQUIRE)) {
        thrd_yield();
    }

    thread_arg->result = get_concurrent_once(77);
    return 0;
}

int
main(void)
{
    init_once();
    init_once();

    if (void_hits != 1) {
        fprintf(stderr, "FAIL: void once ran %d times\n", void_hits);
        return 1;
    }

    int first = get_once();
    int second = get_once();

    if (first != 10 || second != 10 || value_hits != 1) {
        fprintf(stderr, "FAIL: value once first=%d second=%d hits=%d\n",
                first, second, value_hits);
        return 1;
    }

    int param_first = get_param_once(7, 6);
    int param_second = get_param_once(2, 20);

    if (param_first != 42 || param_second != 42 || param_hits != 1
        || param_first_value != 7 || param_first_scale != 6) {
        fprintf(stderr,
                "FAIL: param once first=%d second=%d hits=%d value=%d scale=%d\n",
                param_first, param_second, param_hits, param_first_value,
                param_first_scale);
        return 1;
    }

    if (decorated_once(41) != 42) {
        fprintf(stderr, "FAIL: attributed once name was not preserved\n");
        return 1;
    }

    if (param_attr_once(7, 6) != 42) {
        fprintf(stderr,
                "FAIL: attributed once parameter was not forwarded\n");
        return 1;
    }

    if (interleaved_once() != 1099511627776L) {
        fprintf(stderr, "FAIL: static long once return was not preserved\n");
        return 1;
    }

    int *ptr_first = get_ptr_once();
    int *ptr_second = get_ptr_once();

    if (ptr_first != &pointer_value || ptr_second != &pointer_value
        || pointer_hits != 1 || *ptr_first != 41) {
        fprintf(stderr,
                "FAIL: pointer once first=%p second=%p expected=%p hits=%d\n",
                (void *)ptr_first, (void *)ptr_second,
                (void *)&pointer_value, pointer_hits);
        return 1;
    }

    thrd_t threads[CONCURRENT_THREAD_COUNT];
    concurrent_arg_t args[CONCURRENT_THREAD_COUNT];

    for (int i = 0; i < CONCURRENT_THREAD_COUNT; i++) {
        args[i].result = -1;
        if (thrd_create(&threads[i], call_concurrent_once, &args[i])
            != thrd_success) {
            fprintf(stderr, "FAIL: thrd_create failed for thread %d\n", i);
            return 1;
        }
    }

    while (__atomic_load_n(&concurrent_ready, __ATOMIC_SEQ_CST)
           < CONCURRENT_THREAD_COUNT) {
        thrd_yield();
    }
    __atomic_store_n(&concurrent_release, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < CONCURRENT_THREAD_COUNT; i++) {
        int thread_rc = 0;
        if (thrd_join(threads[i], &thread_rc) != thrd_success
            || thread_rc != 0) {
            fprintf(stderr, "FAIL: thread %d join rc=%d\n", i, thread_rc);
            return 1;
        }
        if (args[i].result != 1077) {
            fprintf(stderr, "FAIL: thread %d saw result %d\n", i,
                    args[i].result);
            return 1;
        }
    }

    if (concurrent_hits != 1) {
        fprintf(stderr, "FAIL: concurrent once ran %d times\n",
                concurrent_hits);
        return 1;
    }

    return 0;
}
