typedef struct ncc_vargs_t {
    unsigned int  nargs;
    unsigned int  cur_ix;
    void        **args;
} ncc_vargs_t;

int
count_args(int base, +)
{
    return base + (int)vargs->nargs;
}

int
main(void)
{
    return count_args(1, .extra = 2);
}
