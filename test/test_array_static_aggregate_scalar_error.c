typedef struct n00b_string_t n00b_string_t;

typedef struct {
    n00b_string_t *name;
    unsigned long  tag;
} entry_t;

typedef _generic_struct typeid("array", entry_t) {
    entry_t *data;
    int      len;
    int      cap;
} entry_array_t;

entry_array_t entries = [{.name = r"one", .tag = 1}];

int
main(void)
{
    return entries.len == 1 ? 0 : 1;
}
