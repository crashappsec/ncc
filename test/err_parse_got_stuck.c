// Deliberate parse error: two type specifiers in one declarator. Exercises the
// parse-failure diagnostic that reports the furthest stuck token's
// file:line:column. See meson.build ncc_err_parse_got_stuck*.
int
main(void)
{
    void *int stuck = 0;
    return 0;
}
