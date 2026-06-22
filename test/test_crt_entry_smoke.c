static int ctor_seen;

[[gnu::constructor]] static void
crt_entry_smoke_ctor(void)
{
    ctor_seen = 1;
}

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return ctor_seen ? 0 : 7;
}
