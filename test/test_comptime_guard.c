#include "parse/comptime_guard.h"

#include "lib/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *
non_host_target(void)
{
#if defined(__APPLE__)
    return "x86_64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__x86_64__)
    return "aarch64-unknown-linux-gnu";
#elif defined(__linux__) && defined(__aarch64__)
    return "x86_64-unknown-linux-gnu";
#elif defined(_WIN32)
    return "x86_64-unknown-linux-gnu";
#else
    return "x86_64-unknown-linux-gnu";
#endif
}

static void
test_target_detection(void)
{
    ncc_opts_t opts = {0};
    assert(ncc_target_is_host(nullptr));
    assert(ncc_target_is_host(&opts));

    const char *host_args[] = { "--target", ncc_host_triple() };
    opts.clang_args = host_args;
    opts.n_clang_args = 2;
    assert(ncc_target_is_host(&opts));

    const char *target_eq_args[] = { "-target=x86_64-unknown-linux-gnu" };
    opts.clang_args = target_eq_args;
    opts.n_clang_args = 1;
#if defined(__linux__) && defined(__x86_64__)
    assert(ncc_target_is_host(&opts));
#else
    assert(!ncc_target_is_host(&opts));
#endif

    const char *non_host_args[] = { "--target", non_host_target() };
    opts.clang_args = non_host_args;
    opts.n_clang_args = 2;
    assert(!ncc_target_is_host(&opts));
}

static void
test_guard_truth_table(void)
{
    const char *non_host_args[] = { "--target", non_host_target() };
    ncc_opts_t opts = {
        .clang_args = non_host_args,
        .n_clang_args = 2,
    };
    ncc_ct_aggregate_t no_main = {0};
    ncc_ct_aggregate_t main_required = {
        .has_comptime_main = true,
        .main_flags = 0,
    };
    ncc_ct_aggregate_t main_optional = {
        .has_comptime_main = true,
        .main_flags = NCC_CT_MAIN_FLAG_OPTIONAL,
    };
    char *err = nullptr;

    assert(ncc_comptime_guard_check(&opts, &no_main, &err));
    assert(err == nullptr);

    assert(!ncc_comptime_guard_check(&opts, &main_required, &err));
    assert(err);
    assert(strstr(err, "cannot run comptime due to platform mismatch"));
    ncc_free(err);
    err = nullptr;

    assert(ncc_comptime_guard_check(&opts, &main_optional, &err));
    assert(err == nullptr);

    opts.no_comptime = true;
    assert(ncc_comptime_guard_check(&opts, &main_required, &err));
    assert(err == nullptr);

    opts.no_comptime = false;
    const char *host_args[] = { "--target", ncc_host_triple() };
    opts.clang_args = host_args;
    opts.n_clang_args = 2;
    assert(ncc_comptime_guard_check(&opts, &main_required, &err));
    assert(err == nullptr);
}

static void
test_static_init_degrade_route(void)
{
    const char *non_host_args[] = { "--target", non_host_target() };
    ncc_opts_t opts = {
        .clang_args = non_host_args,
        .n_clang_args = 2,
    };
    ncc_ct_static_init_t si = {
        .name = NCC_STRING_STATIC("state"),
        .kind = NCC_CT_STATIC_INIT_CONST_RO,
        .flags = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 1,
    };
    ncc_ct_aggregate_t static_only = {
        .static_inits = &si,
        .n_static_inits = 1,
    };
    ncc_comptime_degrade_route_t route =
        ncc_comptime_degrade_route(&opts, &static_only);

    assert(!route.target_is_host);
    assert(route.static_init_degrade);
    assert(!route.comptime_main_degrade);

    char *err = nullptr;
    assert(ncc_static_init_degrade_allowed(&static_only, &err));
    assert(err == nullptr);

    si.degrade_ok = 0;
    assert(!ncc_static_init_degrade_allowed(&static_only, &err));
    assert(err != nullptr);
    assert(strstr(err,
                  "static initializer 'state' cannot be lowered to runtime "
                  "initialization for this target"));
    ncc_free(err);
}

static void
test_no_comptime_static_init_degrade_route(void)
{
    ncc_opts_t opts = {
        .no_comptime = true,
    };
    ncc_ct_static_init_t si = {
        .name = NCC_STRING_STATIC("state"),
        .kind = NCC_CT_STATIC_INIT_CONST_RO,
        .flags = NCC_CT_STATIC_INIT_FLAG_POINTER_ROOT,
        .degrade_ok = 1,
    };
    ncc_ct_aggregate_t static_only = {
        .static_inits = &si,
        .n_static_inits = 1,
    };
    ncc_comptime_degrade_route_t route =
        ncc_comptime_degrade_route(&opts, &static_only);

    assert(route.target_is_host);
    assert(route.static_init_degrade);
    assert(!route.comptime_main_degrade);
}

int
main(void)
{
    test_target_detection();
    test_guard_truth_table();
    test_static_init_degrade_route();
    test_no_comptime_static_init_degrade_route();
    puts("PASS: comptime guard");
    return 0;
}
