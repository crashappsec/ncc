// WP-011 Phase 2: explicit `d{...}` targeting a non-dict type must
// produce the dict-target-mismatch diagnostic before reaching the
// Phase 2 lowering stub.

int x = d{1: 2};
