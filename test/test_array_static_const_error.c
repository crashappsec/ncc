typedef _generic_struct typeid("array", int) {
    int *data;
    int len;
    int cap;
} int_array_t;

const int_array_t state = [1, 2, 3];

int
main(void)
{
    return state.len == 3 ? 0 : 1;
}
