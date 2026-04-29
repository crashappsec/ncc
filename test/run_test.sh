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
#   depfile      — compile with -MMD -MF, expect a populated depfile
#   depfile_md   — -MD -MF targets the requested object path
#   depfile_default — -MMD without -MF writes the default .d path
#   depfile_mt   — depfile honors -MT rule target
#   depfile_mq   — depfile honors -MQ escaped rule target
#   depfile_expect_error — invalid depfile path must make ncc fail
#   compiler_stdin_close — final compiler closes stdin before reading
#   constexpr_runtime_error — runtime helper failures name phase and status
#   cc_fallback_binary_output — CC selects fake compiler when NCC_COMPILER is unset
#   cc_self_guard — CC=ncc is ignored to avoid self-recursion
#   verbose_binary_output — verbose passthrough preserves compiler stdout bytes
#   verbose_high_output — verbose compile drains output while writing stdin
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
    rm -f "$OUTBIN" "$OUTBIN.c" "$OUTBIN.o" "$OUTBIN.d" \
          "$OUTBIN.stdout" "$OUTBIN.stderr" "$OUTBIN.expected"
}
trap cleanup EXIT

check_depfile_mentions_header() {
    if [ ! -s "$OUTBIN.d" ]; then
        echo "FAIL: expected non-empty depfile" >&2
        exit 1
    fi
    if ! grep -q 'dep_header.h' "$OUTBIN.d"; then
        echo "FAIL: depfile does not name dep_header.h" >&2
        cat "$OUTBIN.d" >&2
        exit 1
    fi
}

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

    depfile)
        "$NCC" "$@" -c -MMD -MF "$OUTBIN.d" -o "$OUTBIN.o" "$SRC"
        check_depfile_mentions_header
        ;;

    depfile_md)
        "$NCC" "$@" -c -MD -MF "$OUTBIN.d" -o "$OUTBIN.o" "$SRC"
        check_depfile_mentions_header
        first_rule=$(sed -n '1p' "$OUTBIN.d")
        case "$first_rule" in
            "$OUTBIN.o:"*) ;;
            *)
                echo "FAIL: -MD depfile first rule does not target -o object" >&2
                cat "$OUTBIN.d" >&2
                exit 1
                ;;
        esac
        ;;

    depfile_default)
        "$NCC" "$@" -c -MMD -o "$OUTBIN.o" "$SRC" > "$OUTBIN.stdout"
        if [ -s "$OUTBIN.stdout" ]; then
            echo "FAIL: dependency rule was printed to stdout" >&2
            cat "$OUTBIN.stdout" >&2
            exit 1
        fi
        check_depfile_mentions_header
        ;;

    depfile_mt)
        "$NCC" "$@" -c -MMD -MF "$OUTBIN.d" -MT custom-target \
            -o "$OUTBIN.o" "$SRC"
        check_depfile_mentions_header
        if ! sed -n '1p' "$OUTBIN.d" | grep -q '^custom-target:'; then
            echo "FAIL: depfile first rule does not use -MT target" >&2
            cat "$OUTBIN.d" >&2
            exit 1
        fi
        ;;

    depfile_mq)
        "$NCC" "$@" -c -MMD -MF "$OUTBIN.d" -MQ "space target" \
            -o "$OUTBIN.o" "$SRC"
        check_depfile_mentions_header
        if ! sed -n '1p' "$OUTBIN.d" | grep -q '^space\\ target:'; then
            echo "FAIL: depfile first rule does not use -MQ target" >&2
            cat "$OUTBIN.d" >&2
            exit 1
        fi
        ;;

    depfile_expect_error)
        if "$NCC" "$@" -c -MMD -MF "$OUTBIN.missing/out.d" \
            -o "$OUTBIN.o" "$SRC" 2>/dev/null; then
            echo "FAIL: expected depfile generation failure" >&2
            exit 1
        fi
        if [ -e "$OUTBIN.o" ]; then
            echo "FAIL: object file exists after depfile failure" >&2
            exit 1
        fi
        ;;

    constexpr_runtime_error)
        set +e
        "$NCC" "$@" -o "$OUTBIN" "$SRC" 2> "$OUTBIN.stderr"
        STATUS=$?
        set -e
        if [ "$STATUS" -eq 0 ]; then
            echo "FAIL: expected constexpr runtime failure" >&2
            exit 1
        fi
        if ! grep -q 'helper execution failed' "$OUTBIN.stderr"; then
            echo "FAIL: diagnostic does not name helper execution phase" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -q 'exit status' "$OUTBIN.stderr"; then
            echo "FAIL: diagnostic does not include exit status" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if grep -q 'constexpr: compilation failed' "$OUTBIN.stderr"; then
            echo "FAIL: runtime failure was labelled as compilation failure" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    cc_fallback_binary_output)
        if [ -z "$NCC_TEST_CC" ]; then
            echo "FAIL: NCC_TEST_CC is required for cc_fallback_binary_output" >&2
            exit 1
        fi
        printf 'A\000B' > "$OUTBIN.expected"
        env -u NCC_COMPILER CC="$NCC_TEST_CC" NCC_VERBOSE=1 \
            "$NCC" --no-ncc "$@" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if ! cmp -s "$OUTBIN.expected" "$OUTBIN.stdout"; then
            echo "FAIL: CC fallback compiler stdout bytes changed" >&2
            exit 1
        fi
        if ! grep -q 'using CC=' "$OUTBIN.stderr"; then
            echo "FAIL: verbose output did not show CC fallback" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    cc_self_guard)
        env -u NCC_COMPILER CC="$NCC" NCC_VERBOSE=1 \
            "$NCC" "$@" -o "$OUTBIN" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN"
        if ! grep -q 'ignoring CC=' "$OUTBIN.stderr"; then
            echo "FAIL: verbose output did not show CC self-recursion guard" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    compiler_stdin_close)
        set +e
        NCC_VERBOSE=1 "$NCC" "$@" -c -o "$OUTBIN.o" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        STATUS=$?
        set -e
        if [ "$STATUS" -ne 42 ]; then
            echo "FAIL: expected final compiler exit 42, got $STATUS" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -q 'fake final compiler closed stdin before read' \
            "$OUTBIN.stderr"; then
            echo "FAIL: final compiler diagnostic was not preserved" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if grep -q 'failed to launch compiler' "$OUTBIN.stderr"; then
            echo "FAIL: post-launch stdin close was reported as launch failure" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    verbose_binary_output)
        printf 'A\000B' > "$OUTBIN.expected"
        NCC_VERBOSE=1 "$NCC" --no-ncc "$@" "$SRC" > "$OUTBIN.stdout"
        if ! cmp -s "$OUTBIN.expected" "$OUTBIN.stdout"; then
            echo "FAIL: verbose compiler stdout bytes changed" >&2
            echo "expected:" >&2
            od -An -tx1 -v "$OUTBIN.expected" >&2
            echo "actual:" >&2
            od -An -tx1 -v "$OUTBIN.stdout" >&2
            exit 1
        fi
        ;;

    verbose_high_output)
        NCC_VERBOSE=1 "$NCC" "$@" -c -o "$OUTBIN.o" "$SRC" \
            > "$OUTBIN.stdout"
        if [ ! -s "$OUTBIN.o" ]; then
            echo "FAIL: expected fake object output" >&2
            exit 1
        fi
        if [ ! -s "$OUTBIN.stdout" ]; then
            echo "FAIL: expected verbose compiler stdout" >&2
            exit 1
        fi
        ;;

    *)
        echo "unknown mode: $MODE" >&2
        exit 1
        ;;
esac
