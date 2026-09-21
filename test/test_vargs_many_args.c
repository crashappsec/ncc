// Regression: the source text the kargs/vargs transform generates has no
// size ceiling.
//
// The transform builds three pieces of C as strings: the rewritten call
// expression, the _kargs struct declaration, and the kargs compound literal.
// A call with enough vargs, or a _kargs list with enough params, makes one of
// them longer than any fixed array; a write past the end of such an array is
// silent for a while and then overwrites a return address.
//
// `typeid_pad` sits among the vargs so the generated call text reaches the
// typeid/typehash resolver's scanning path. Absent a "typeid" or "typehash"
// substring the resolver copies its input and returns early, skipping the
// buffer this fixture is here to size-check.

#include <stdio.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

static int sum_vargs(int base, +) {
    int total = base;
    for (unsigned i = 0; i < vargs->nargs; i++) {
        total += *(int *)vargs->args[i];
    }
    return total;
}

#define REP2(x)    x, x
#define REP4(x)    REP2(x), REP2(x)
#define REP8(x)    REP4(x), REP4(x)
#define REP16(x)   REP8(x), REP8(x)
#define REP32(x)   REP16(x), REP16(x)
#define REP64(x)   REP32(x), REP32(x)
#define REP128(x)  REP64(x), REP64(x)
#define REP256(x)  REP128(x), REP128(x)
#define REP512(x)  REP256(x), REP256(x)
#define REP1024(x) REP512(x), REP512(x)

// 300 keyword params, so the struct text and its _has_ bitfields together run
// well past 8K.
#define KPARAM(name) int name = 1;

#define EACH_10(M, p) \
    M(p##0) M(p##1) M(p##2) M(p##3) M(p##4) M(p##5) M(p##6) M(p##7) M(p##8) \
        M(p##9)

#define EACH_100(M, p)                                                  \
    EACH_10(M, p##0) EACH_10(M, p##1) EACH_10(M, p##2) EACH_10(M, p##3) \
    EACH_10(M, p##4) EACH_10(M, p##5) EACH_10(M, p##6) EACH_10(M, p##7) \
    EACH_10(M, p##8) EACH_10(M, p##9)

#define KPARAMS \
    EACH_100(KPARAM, kp_a) EACH_100(KPARAM, kp_b) EACH_100(KPARAM, kp_c)

int wide_kargs(int x) _kargs { KPARAMS };
int wide_kargs(int x) _kargs { KPARAMS } {
    return x + kargs->kp_a00 + kargs->kp_b55 + kargs->kp_c99;
}

int main(void) {
    int one        = 1;
    int typeid_pad = 0;

    int total = sum_vargs(0, REP1024(&one), &typeid_pad);
    if (total != 1024) {
        printf("FAIL vargs total=%d want 1024\n", total);
        return 1;
    }

    if (wide_kargs(10) != 13) {
        printf("FAIL kargs defaults=%d want 13\n", wide_kargs(10));
        return 2;
    }
    if (wide_kargs(10, .kp_b55 = 5) != 17) {
        printf("FAIL kargs override=%d want 17\n", wide_kargs(10, .kp_b55 = 5));
        return 3;
    }

    printf("PASS vargs/kargs generated source has no ceiling\n");
    return 0;
}
