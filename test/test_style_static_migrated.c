#include "n00b.h"
#include "adt/dict.h"
#include "core/gc.h"
#include "text/strings/text_style.h"

static const n00b_text_style_t style_em = {
    .font_index    = -1,
    .fg_palette_ix = -1,
    .bg_palette_ix = -1,
    .italic        = N00B_TRI_YES,
};

static const n00b_text_style_t style_code = {
    .font_index    = -1,
    .fg_palette_ix = -1,
    .bg_palette_ix = -1,
    .font_hint     = N00B_FONT_MONO,
};

static const n00b_text_style_t *
pick_em_style(void)
{
    return &style_em;
}

n00b_dict_t(n00b_string_t *, const n00b_text_style_t *) builtin_named_styles = d{
    r"em": &style_em,
};

n00b_dict_t(n00b_string_t *, const n00b_text_style_t *) builtin_roles = d{
    r"@code": &style_code,
};

n00b_dict_t(n00b_string_t *, const n00b_text_style_t *) computed_named_styles = d{
    r"em": pick_em_style(),
    r"code": &style_code,
};

static int
expect_style(n00b_dict_t(n00b_string_t *, const n00b_text_style_t *) *dict,
             n00b_string_t *key, const n00b_text_style_t *expect,
             bool require_identity, bool reject_identity)
{
    bool                     found = false;
    const n00b_text_style_t *value = n00b_dict_get(dict, key, &found);

    if (!found) {
        return 1;
    }
    if (require_identity && value != expect) {
        return 2;
    }
    if (reject_identity && value == expect) {
        return 5;
    }
    if (value == nullptr) {
        return 3;
    }
    if (value->bold != expect->bold
        || value->italic != expect->italic
        || value->underline != expect->underline
        || value->font_hint != expect->font_hint
        || value->font_index != expect->font_index
        || value->fg_palette_ix != expect->fg_palette_ix
        || value->bg_palette_ix != expect->bg_palette_ix) {
        return 4;
    }
    return 0;
}

static int
check_style_dict(n00b_dict_t(n00b_string_t *, const n00b_text_style_t *) *dict,
                 n00b_gc_scan_kind_t value_scan_kind)
{
    if (dict->lock != 0 || dict->store == nullptr) {
        return 10;
    }
    if (dict->skip_obj_hash
        || dict->key_scan_kind != N00B_GC_SCAN_KIND_ALL
        || dict->value_scan_kind != value_scan_kind) {
        return 11;
    }
    return 0;
}

int
main(void)
{
    int rc = check_style_dict(&builtin_named_styles, N00B_GC_SCAN_KIND_NONE);
    if (rc != 0) {
        return rc;
    }
    rc = check_style_dict(&builtin_roles, N00B_GC_SCAN_KIND_NONE);
    if (rc != 0) {
        return 20 + rc;
    }
    rc = check_style_dict(&computed_named_styles, N00B_GC_SCAN_KIND_ALL);
    if (rc != 0) {
        return 30 + rc;
    }

    rc = expect_style(&builtin_named_styles, r"em", &style_em, true, false);
    if (rc != 0) {
        return 40 + rc;
    }
    rc = expect_style(&builtin_roles, r"@code", &style_code, true, false);
    if (rc != 0) {
        return 50 + rc;
    }
    rc = expect_style(&computed_named_styles, r"em", &style_em, false, true);
    if (rc != 0) {
        return 60 + rc;
    }
    rc = expect_style(&computed_named_styles, r"code", &style_code, false, true);
    if (rc != 0) {
        return 70 + rc;
    }

    return 0;
}
