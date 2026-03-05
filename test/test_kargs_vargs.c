#include <stdio.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

// Function with both vargs and kargs
void log_msg(int level, +) _kargs { const char *prefix = "LOG"; } {
    printf("[%s] level=%d args=%u\n", prefix, level, vargs->nargs);
}

int main(void) {
    log_msg(1);                                // no vargs, default prefix
    log_msg(2, "a", "b", .prefix = "DBG");     // vargs + karg override
    return 0;
}
