#include <stdio.h>

typedef struct ncc_buffer_t {
    int value;
} ncc_buffer_t;

#define TYPED_WRITE_FN(T) typeid("typed_write", T)
#define typed_write(T, payload, ...) TYPED_WRITE_FN(T)(payload, ##__VA_ARGS__)

static int
TYPED_WRITE_FN(ncc_buffer_t *)(ncc_buffer_t *payload) _kargs { int delta = 1; }
{
    return payload->value + delta;
}

int
main(void)
{
    ncc_buffer_t payload = {.value = 4};
    int          result  = typed_write(ncc_buffer_t *, &payload, .delta = 5);

    if (result != 9) {
        printf("FAIL result=%d\n", result);
        return 1;
    }

    return 0;
}
