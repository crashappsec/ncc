typedef int (*init_fn)(void);

[[gnu::used, gnu::section(".n00bsi$a")]]
static init_fn const init_begin = 0;
[[gnu::used, gnu::section(".n00bsi$z")]]
static init_fn const init_end = 0;

static int prepared;

int
__ncc_static_init_prepare_state(void)
{
    prepared++;
    return 0;
}

[[gnu::used, gnu::section(".n00bct")]]
static const unsigned char comptime_metadata[] = {
    0x4e, 0x30, 0x43, 0x54, 0x04, 0x00,
    0x03, 0x00, 0x12, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x01, 0x05, 0x00, 0x73, 0x74, 0x61, 0x74, 0x65,
    0x00, 0x00, 0x00, 0x00,
};

int
main(void)
{
    if (prepared != 0) {
        return 8;
    }
    for (const init_fn *it = &init_begin + 1; it < &init_end; it++) {
        if (*it) {
            int rc = (*it)();
            if (rc != 0) {
                return rc;
            }
        }
    }
    return prepared == 1 ? 0 : 9;
}
