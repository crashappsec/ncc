#include <stdio.h>
#include <string.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

static unsigned    seen_args[4];
static const char *seen_prefix[4];
static int         seen_count;

// Function with both vargs and kargs
void log_msg(int level, +) _kargs { const char *prefix = "LOG"; } {
    seen_prefix[seen_count] = prefix;
    seen_args[seen_count]   = vargs->nargs;
    seen_count++;
    printf("[%s] level=%d args=%u\n", prefix, level, vargs->nargs);
}

int main(void) {
    log_msg(1);                                // no vargs, default prefix
    log_msg(2, "a", "b", .prefix = "DBG");     // vargs + karg override
    log_msg(3, "a", "b");                      // vargs + omitted kargs

    if (seen_count != 3) {
        printf("FAIL seen_count=%d\n", seen_count);
        return 1;
    }
    if (seen_args[0] != 0 || strcmp(seen_prefix[0], "LOG") != 0) {
        printf("FAIL default no-vargs case\n");
        return 2;
    }
    if (seen_args[1] != 2 || strcmp(seen_prefix[1], "DBG") != 0) {
        printf("FAIL keyword override case\n");
        return 3;
    }
    if (seen_args[2] != 2 || strcmp(seen_prefix[2], "LOG") != 0) {
        printf("FAIL omitted kargs with two vargs\n");
        return 4;
    }
    return 0;
}
