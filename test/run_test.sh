#!/bin/sh
# NCC compiler smoke test runner.
#
# Usage: run_test.sh <ncc> <mode> <source> [ncc-flags...]
#
# Modes:
#   compile_run  — compile source, run the binary, expect exit 0
#   preprocess   — run with -E, expect exit 0 and non-empty output
#   no_ncc       — compile with --no-ncc, run the binary, expect exit 0
#   expect_error — compile, expect non-zero exit
#
set -e

NCC="$1"
MODE="$2"
SRC="$3"
shift 3 || true

if [ -z "$NCC" ] || [ -z "$MODE" ] || [ -z "$SRC" ]; then
    echo "usage: run_test.sh <ncc> <mode> <source> [ncc-flags...]" >&2
    exit 1
fi

TMPDIR="${MESON_BUILD_ROOT:-/tmp}"
OUTBIN="$TMPDIR/ncc_test_$$"

cleanup() {
    rm -f "$OUTBIN" "$OUTBIN.c"
}
trap cleanup EXIT

case "$MODE" in
    compile_run)
        "$NCC" "$@" -o "$OUTBIN" "$SRC"
        "$OUTBIN"
        ;;

    preprocess)
        OUTPUT=$("$NCC" "$@" -E "$SRC")
        if [ -z "$OUTPUT" ]; then
            echo "FAIL: -E produced empty output" >&2
            exit 1
        fi
        ;;

    no_ncc)
        "$NCC" --no-ncc "$@" -o "$OUTBIN" "$SRC"
        "$OUTBIN"
        ;;

    expect_error)
        if "$NCC" "$@" -o "$OUTBIN" "$SRC" 2>/dev/null; then
            echo "FAIL: expected non-zero exit from ncc" >&2
            exit 1
        fi
        ;;

    *)
        echo "unknown mode: $MODE" >&2
        exit 1
        ;;
esac
