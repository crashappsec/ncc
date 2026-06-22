#include "n00b.h"
#include "adt/array.h"
#include "adt/list.h"
#include "core/buffer.h"

#include <string.h>

const n00b_buffer_t *buf = b"wp006";
n00b_array_t(int) arr = [1, 2, 3];
n00b_list_t(int) list = l{4, 5, 6};

int
main(void)
{
    if (buf == nullptr || buf->data == nullptr || buf->byte_len != 5
        || memcmp(buf->data, "wp006", 5) != 0) {
        return 10;
    }
    if (arr.len != 3 || arr.cap != 3 || arr.data == nullptr
        || arr.data[0] != 1 || arr.data[2] != 3) {
        return 20;
    }
    if (list.len != 3 || list.cap != 16 || list.lock == nullptr
        || n00b_list_get(list, 0) != 4 || n00b_list_get(list, 2) != 6) {
        return 30;
    }
    return 0;
}
