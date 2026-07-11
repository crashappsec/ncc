// Deliberate parse error: input ends before the translation unit completes.
// Exercises the parse-failure diagnostic's unexpected-EOF branch.
// See meson.build ncc_err_parse_eof.
int
main(void)
{
