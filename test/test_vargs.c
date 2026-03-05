#include <stdio.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

int sum(int count, +) {
    int total = count;
    for (unsigned i = 0; i < vargs->nargs; i++) {
        total += *(int *)vargs->args[i];
    }
    return total;
}

int main(void) {
    int a = 1, b = 2, c = 3;
    int r1 = sum(0);                // no varargs
    int r2 = sum(0, &a, &b, &c);   // 3 varargs (pass addresses since args is void*[])
    printf("%d %d\n", r1, r2);
    return (r1 != 0 || r2 != 6);
}
