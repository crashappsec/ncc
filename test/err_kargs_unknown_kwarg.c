int
wait_ms(int fd) _kargs
{
    long timeout_ms = 0;
};

int
wait_ms(int fd) _kargs
{
    long timeout_ms = 0;
}
{
    return fd + (int)timeout_ms;
}

int
main(void)
{
    return wait_ms(0, .timeout = 50);
}
