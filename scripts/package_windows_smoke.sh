#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <build-dir> <out-dir>" >&2
    exit 1
fi

BUILD_DIR=$1
OUT_DIR=$2

if [ ! -f "$BUILD_DIR/ncc.exe" ]; then
    echo "missing $BUILD_DIR/ncc.exe" >&2
    exit 1
fi
if [ ! -f "$BUILD_DIR/test_process_run.exe" ]; then
    echo "missing $BUILD_DIR/test_process_run.exe" >&2
    exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

cp "$BUILD_DIR/ncc.exe" "$OUT_DIR/"
cp "$BUILD_DIR/test_process_run.exe" "$OUT_DIR/"
if [ -f "$BUILD_DIR/libncc.a" ]; then
    cp "$BUILD_DIR/libncc.a" "$OUT_DIR/"
fi

cp test/windows_smoke.ps1 "$OUT_DIR/"
cp test/test_bang.c "$OUT_DIR/"
cp test/test_option.c "$OUT_DIR/"
cp test/test_constexpr.c "$OUT_DIR/"

cat <<EOF
Windows smoke bundle created at: $OUT_DIR

Copy the bundle to Windows. From inside that directory, run:
  \$env:NCC_COMPILER='clang'; & .\windows_smoke.ps1 -Ncc .\ncc.exe

If clang.exe is not on PATH, set NCC_COMPILER to its full path instead.
The script enables NCC_VERBOSE=1 by default unless you set it first.
The script writes its own transcript to .\windows-smoke-transcript.txt.
Send that transcript file back after the run.
EOF
