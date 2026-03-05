// Simulates a second header that uses ncc_result_t(int).
// This would fail with ncc_result_decl() if both headers are included,
// but _generic_struct deduplicates automatically.

#pragma once

#include "test_gs_result.h"

// This expansion produces _generic_struct <tag> { ... } a second time.
// _generic_struct deduplicates it — only the first definition survives.
static inline ncc_result_t(int)
double_result(ncc_result_t(int) r)
{
    if (ncc_result_is_ok(r)) {
        return ncc_result_ok(int, ncc_result_get(r) * 2);
    }
    return r;
}
