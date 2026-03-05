// Broken bang: used at file scope (not in a function).

typedef struct {
    bool is_ok;
    int  ok;
    int  err;
} int_result_t;

int_result_t get_value(void);

// Bug: ! at file scope — no enclosing function to return from
int x = get_value()!;

int main(void) { return 0; }
