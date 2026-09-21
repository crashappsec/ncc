// Vargs whose declared type is a STRUCT passed BY VALUE.
//
// A value wider than void* cannot sit in the `void *args[]` array directly,
// so the transform copies each one and stores the copy's address. Each copy
// must be a distinct object that outlives the call. Getting that wrong is
// invisible at -O0, where a dead stack slot usually still holds the right
// bytes, and silently corrupts every argument but the last at -O2, where the
// slot gets reused. So this test checks the addresses are distinct, not just
// that the values read back correctly.
//
// Optimization is therefore part of the fixture, not a build detail: at -O0
// this file passes whether or not the defect is present. Fail the build
// rather than report a green test if the flag stops arriving. MSVC defines
// neither macro, so it is exempt.
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
#error "test_vargs_struct_byvalue needs -O1 or higher to detect anything"
#endif

#include <stdio.h>
#include <string.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

// Deliberately wider than a pointer, so it takes the by-value path.
typedef struct {
    const char *name;
    int         value;
    long        pad;
} field_t;

static field_t mk_field(const char *name, int value) {
    return (field_t){ .name = name, .value = value, .pad = value * 2 };
}

static int  seen_count;
static char seen_names[4][8];
static int  seen_values[4];
static int  distinct_addrs;

static void take_fields(int tag, field_t +) {
    (void)tag;
    seen_count = (int)vargs->nargs;

    for (unsigned i = 0; i < vargs->nargs && i < 4; i++) {
        field_t *f = (field_t *)vargs->args[i];
        snprintf(seen_names[i], sizeof(seen_names[i]), "%s", f->name);
        seen_values[i] = f->value;
    }

    // Every copy must be its own object.
    distinct_addrs = 1;
    for (unsigned i = 0; i < vargs->nargs; i++) {
        for (unsigned j = i + 1; j < vargs->nargs; j++) {
            if (vargs->args[i] == vargs->args[j]) {
                distinct_addrs = 0;
            }
        }
    }
}

int main(void) {
    take_fields(7,
                mk_field("x", 10),
                mk_field("y", 20),
                mk_field("z", 30));

    if (seen_count != 3) {
        printf("FAIL nargs=%d want 3\n", seen_count);
        return 1;
    }
    if (!distinct_addrs) {
        printf("FAIL vargs entries alias: the copies share storage\n");
        return 2;
    }
    if (strcmp(seen_names[0], "x") != 0 || strcmp(seen_names[1], "y") != 0
        || strcmp(seen_names[2], "z") != 0) {
        printf("FAIL names=[%s, %s, %s] want [x, y, z]\n",
               seen_names[0], seen_names[1], seen_names[2]);
        return 3;
    }
    if (seen_values[0] != 10 || seen_values[1] != 20 || seen_values[2] != 30) {
        printf("FAIL values=[%d, %d, %d] want [10, 20, 30]\n",
               seen_values[0], seen_values[1], seen_values[2]);
        return 4;
    }

    printf("PASS vargs struct by-value\n");
    return 0;
}
