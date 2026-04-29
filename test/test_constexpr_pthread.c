#include <pthread.h>
#include <stdio.h>

typedef struct {
    pthread_t thread;
    int       value;
} with_pthread_t;

int
main(void)
{
    int result = constexpr_eval(sizeof(with_pthread_t));
    int expected = (int)sizeof(with_pthread_t);

    if (result != expected) {
        fprintf(stderr, "FAIL: pthread constexpr size=%d expected=%d\n",
                result, expected);
        return 1;
    }

    return 0;
}
