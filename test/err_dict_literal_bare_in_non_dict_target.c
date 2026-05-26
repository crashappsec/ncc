// WP-011 Phase 2: bare `{key: value}` form is parsed as a dict
// literal (D-063), so a non-dict target must still produce the dict-
// target-mismatch diagnostic.  This is the bare-form counterpart to
// err_dict_literal_non_dict_target.c.

int x = {1: 2};
