extern void *make_static_state(void);

static const void *state = make_static_state();

int
main(void)
{
    return state == nullptr;
}
