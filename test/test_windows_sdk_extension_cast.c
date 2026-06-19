typedef long long __m64 __attribute__((__vector_size__(8), __aligned__(8)));
typedef int __v2si __attribute__((__vector_size__(8)));

static __m64
sdk_intrinsic_shape(void)
{
    return __extension__ (__m64)(__v2si){0, 0};
}

static void
sdk_half_literal_shape(void)
{
    (void)-0.0f16;
}

int
main(void)
{
    (void)sdk_intrinsic_shape();
    sdk_half_literal_shape();
    return 0;
}
