#!/usr/bin/env bash
# Build and optionally test ncc inside a Linux Docker container.
#
# Usage:
#   bash docker/linux-build.sh                          # build only
#   NCC_TEST=1 bash docker/linux-build.sh               # build + test
#   NCC_CLEAN=1 NCC_TEST=1 bash docker/linux-build.sh   # clean rebuild + test
#   NCC_JOBS=4 bash docker/linux-build.sh               # limit parallelism (default: 2)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="ncc-linux"

echo "=== Building Docker image ($IMAGE_NAME) ==="
# Ensure sdk/ dir exists (Dockerfile COPY requires it, even when empty)
mkdir -p "$SCRIPT_DIR/sdk"
docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"

echo "=== Running build inside container ==="
docker run --rm \
    -v "$PROJECT_ROOT:/src:ro" \
    -e NCC_TEST="${NCC_TEST:-0}" \
    -e NCC_CLEAN="${NCC_CLEAN:-0}" \
    -e NCC_BUILD_TYPE="${NCC_BUILD_TYPE:-debug}" \
    -e NCC_JOBS="${NCC_JOBS:-2}" \
    -e CC=clang \
    "$IMAGE_NAME" \
    bash -c '
        cp -a /src /build && cd /build

        BUILD_TYPE="${NCC_BUILD_TYPE:-debug}"
        JOBS="${NCC_JOBS:-2}"

        # Always start clean — the copied source tree may contain
        # a host build/ directory from a different meson version.
        rm -rf build
        meson setup --buildtype="$BUILD_TYPE" build .

        meson compile -C build -j "$JOBS"

        if [ "${NCC_TEST:-0}" != "0" ]; then
            echo "=== Running tests ==="
            meson test -C build --print-errorlogs
        fi

        echo "=== Build complete ==="
    '
