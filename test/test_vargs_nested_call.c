#include <stdio.h>
#include <string.h>

typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

/* Regression: a `+`-variadic call nested directly as an argument of another
 * `+`-variadic call must be vargs-packed exactly once. The outer call's
 * rewrite re-parses its replacement text and transforms the inner call via
 * rewrite_nested_kargs_calls; the engine then traverses the replacement
 * subtree and visits the inner call AGAIN. Without an idempotency check for
 * vargs-only callees, the second visit re-bundled the inner call's already-
 * synthesized `&(ncc_vargs_t){...}` literal as if it were a user argument,
 * so the inner callee saw nargs == 1 with args[0] pointing at a vargs
 * struct instead of the real argument (the `n00b_print(n00b_cformat(...))`
 * `<bad-string-arg>` shape). */

static const char *
pick_only(const char *tag, +)
{
    (void)tag;
    if (vargs->nargs != 1) {
        return "WRONG-NARGS";
    }
    return (const char *)vargs->args[0];
}

int
main(void)
{
    const char *hello = "HELLO";

    /* Unnested baseline. */
    const char *flat = pick_only("flat", (void *)hello);

    /* The regression shape: variadic call directly as a variadic argument.
     * Double-packing makes the INNER call return a pointer to a synthesized
     * vargs struct rather than `hello`. */
    const char *nested = pick_only("outer",
                                   (void *)pick_only("inner", (void *)hello));

    printf("%s %s\n", flat, nested);
    return (strcmp(flat, "HELLO") != 0 || strcmp(nested, "HELLO") != 0);
}
