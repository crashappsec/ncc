#include "n00b.h"
#include "adt/dict.h"
#include "core/gc.h"

#include <stdint.h>

typedef struct {
    uint32_t tag;
} marker_t;

static const marker_t marker = {.tag = 7};

static const marker_t *
pick_marker(void)
{
    return &marker;
}

n00b_dict_t(uint64_t, const marker_t *) dict = d{
    1: pick_marker(),
};

n00b_dict_t(uint64_t, marker_t const *) suffix_const_dict = d{
    1: pick_marker(),
};

static int
expect_snapshot(n00b_dict_t(uint64_t, const marker_t *) *candidate)
{
    __n00b_internal_type_erased_store_t *store =
        n00b_pinref_unsafe((n00b_pinref_t *)&candidate->store_pin);

    if (candidate->length != 1
        || store == nullptr
        || store->values == nullptr) {
        return 1;
    }
    if (candidate->value_scan_kind != N00B_GC_SCAN_KIND_ALL) {
        return 2;
    }

    const marker_t **values = (const marker_t **)store->values;
    for (uint32_t i = 0; i <= store->last_slot; i++) {
        if (values[i] != nullptr) {
            if (values[i] == &marker) {
                return 3;
            }
            return values[i]->tag == 7 ? 0 : 4;
        }
    }

    return 5;
}

int
main(void)
{
    int rc = expect_snapshot(&dict);
    if (rc != 0) {
        return rc;
    }
    rc = expect_snapshot((n00b_dict_t(uint64_t, const marker_t *) *)&suffix_const_dict);
    if (rc != 0) {
        return 10 + rc;
    }

    return 0;
}
