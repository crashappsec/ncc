#!/usr/bin/env bash
set -eo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${NCC_BUILD_DIR:-${ROOT_DIR}/build}"

if [[ -z "${SDKROOT:-}" && "$(uname -s)" == "Darwin" ]] \
    && command -v xcrun >/dev/null 2>&1; then
    sdkroot="$(xcrun --show-sdk-path 2>/dev/null || true)"
    if [[ -n "${sdkroot}" ]]; then
        export SDKROOT="${sdkroot}"
    fi
fi

split_words() {
    local input="${1:-}"
    if [[ -z "${input}" ]]; then
        return
    fi
    # shellcheck disable=SC2206
    local words=(${input})
    printf '%s\n' "${words[@]}"
}

setup_args=()
compile_args=()
test_args=(--print-errorlogs)

while IFS= read -r arg; do
    setup_args+=("${arg}")
done < <(split_words "${NCC_SETUP_ARGS:-}")

while IFS= read -r arg; do
    compile_args+=("${arg}")
done < <(split_words "${NCC_COMPILE_ARGS:-}")

while IFS= read -r arg; do
    test_args+=("${arg}")
done < <(split_words "${NCC_TEST_ARGS:-}")

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    meson setup "${setup_args[@]}" "${BUILD_DIR}" "${ROOT_DIR}"
elif [[ "${NCC_RECONFIGURE:-0}" != "0" ]]; then
    meson setup --reconfigure "${setup_args[@]}" "${BUILD_DIR}"
fi

meson compile -C "${BUILD_DIR}" "${compile_args[@]}"

if [[ "${NCC_TEST:-1}" != "0" ]]; then
    meson test -C "${BUILD_DIR}" "${test_args[@]}"
fi
