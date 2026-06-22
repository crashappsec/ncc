typedef struct n00b_string_t n00b_string_t;

typedef _generic_struct typeid("array", n00b_string_t *) {
    n00b_string_t **data;
    int             len;
    int             cap;
} string_array_t;

string_array_t words = [r"[|b|]alpha[|/b|]"];

int
main(void)
{
    return words.len == 1 ? 0 : 1;
}
