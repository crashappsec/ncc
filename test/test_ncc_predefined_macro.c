// __NCC__ must be predefined when a source is compiled through ncc.
#ifndef __NCC__
#error "__NCC__ is not defined under ncc"
#endif

#if __NCC__ != 1
#error "__NCC__ should expand to 1"
#endif

int main(void)
{
    return 0;
}
