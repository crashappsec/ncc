#!/bin/sh
# NCC compiler smoke test runner.
#
# Usage: run_test.sh <ncc> <mode> <source> [ncc-flags...]
#
# Modes:
#   compile_run  — compile source, run the binary, expect exit 0
#   comptime_section_present — compile -c and expect D-028 section + magic
#   comptime_section_absent — compile -c and expect no D-028 section / magic
#   comptime_var_record_present — compile -c and expect the Phase-2 VAR TLV
#   preprocess   — run with -E, expect exit 0 and non-empty output
#   preprocess_contains — run with -E, expect output to contain substring
#   preprocess_not_contains — run with -E, expect output to omit substring
#   preprocess_stderr_contains — run with -E, expect exit 0 AND stderr substring
#                                 AND a second substring absent from stdout
#   preprocess_stderr_omits — run with -E, expect exit 0 AND stderr to omit
#                              a substring (a safe pattern must not warn)
#   no_ncc       — compile with --no-ncc, run the binary, expect exit 0
#   expect_error — compile, expect non-zero exit
#   expect_error_contains — compile, expect non-zero exit and stderr substring
#   expect_error_contains_no_output — compile, expect non-zero exit, stderr
#                                     substring, and no requested output file
#   depfile      — compile with -MMD -MF, expect a populated depfile
#   depfile_md   — -MD -MF targets the requested object path
#   depfile_default — -MMD without -MF writes the default .d path
#   depfile_mt   — depfile honors -MT rule target
#   depfile_mq   — depfile honors -MQ escaped rule target
#   depfile_expect_error — invalid depfile path must make ncc fail
#   compiler_temp_source — final compiler receives transformed C as a temp file
#   constexpr_runtime_error — runtime helper failures name phase and status
#   cc_fallback_binary_output — CC selects fake compiler when NCC_COMPILER is unset
#   cc_self_guard — CC=ncc is ignored to avoid self-recursion
#   verbose_binary_output — verbose passthrough preserves compiler stdout bytes
#   verbose_high_output — verbose compile drains output while writing stdin
#   custom_entry_run — link/run with --ncc-custom-entry using
#                       NCC_CRT_TEST_LDFLAGS for libn00b/runtime inputs
#   custom_entry_object_run — object-only link/run with --ncc-custom-entry
#   custom_entry_default_no_inject — default link does not inject crt flags
#   custom_entry_compile_only_no_inject — -c does not inject crt flags
#   custom_entry_assemble_only_no_inject — -S does not inject crt flags
#   custom_entry_syntax_only_no_inject — -fsyntax-only does not inject crt flags
#   custom_entry_dep_only_no_inject — -M does not inject crt flags
#   comptime_smoke — source+link comptime run once, final binary strips D-028
#   comptime_object_link — object-only comptime link-twice path
#   comptime_mixed_link — source+prebuilt comptime object link-twice path
#   comptime_link_order — source object stays before following static archive
#   comptime_forced_include — source+link comptime handles -include once
#   comptime_fail — nonzero comptime_main aborts the build
#   comptime_atomic_exit — nonzero comptime_main preserves prior output
#   comptime_atomic_signal — signaled comptime_main preserves prior output
#   comptime_no_comptime — --ncc-no-comptime degrades comptime to runtime
#   comptime_optional_metadata — optional main flag survives object metadata
#   comptime_degrade — source+link and object-only --ncc-no-comptime DEGRADE
#   comptime_xcompile_guard — non-host comptime target fails before run/link
#   comptime_var_link_strip — var-only source link strips final D-028 metadata
#   comptime_var_object_link_strip — var-only object link strips final D-028 metadata
#   comptime_image_e2e — link against staged n00b runtime and assert real image
#                        capture/emit/relocate for pointer-root comptime vars;
#                        NCC_TEST_N00B_ROOT overrides auto-discovery, legacy
#                        N00B_WP005_ROOT/N00B_WP003_ROOT still work, and
#                        NCC_TEST_N00B_BUILD_DIR overrides root/build_debug
#   static_init_mutation_error — staged n00b link must fail D-024 when
#                                comptime_main mutates a static-init root
#   static_init_no_comptime_degrade — staged static-init links lower
#                                     --ncc-no-comptime to runtime init
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
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
NCC_SOURCE_ROOT=$(CDPATH= cd -- "$TEST_DIR/.." && pwd -P)

cleanup() {
    if [ "${NCC_TEST_KEEP:-0}" != "0" ]; then
        echo "KEEP: $OUTBIN" >&2
        return
    fi
    rm -f "$OUTBIN" "$OUTBIN.c" "$OUTBIN.o" "$OUTBIN.s" "$OUTBIN.d" \
          "$OUTBIN.stdout" "$OUTBIN.stderr" "$OUTBIN.expected" \
          "$OUTBIN.dump" "$OUTBIN.contents" "$OUTBIN.strings" \
          "$OUTBIN.rt.c" "$OUTBIN.rt.o" "$OUTBIN.start.S" \
          "$OUTBIN.start.o" "$OUTBIN.user.o" "$OUTBIN.dep.c" \
          "$OUTBIN.dep.o" "$OUTBIN.libdep.a" "$OUTBIN.force.h" \
          "$OUTBIN.sentinel"
}
trap cleanup EXIT

assert_no_custom_entry_injection() {
    if grep -F -q -- '-nostartfiles' "$OUTBIN.stderr"; then
        echo "FAIL: no-link custom entry unexpectedly passed -nostartfiles" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
    if grep -F -q -- 'custom crt entry compiler argv' "$OUTBIN.stderr"; then
        echo "FAIL: no-link custom entry unexpectedly compiled generated crt entry" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
}

assert_custom_entry_target_forwarding() {
    case " $NCC_CRT_TEST_LDFLAGS " in
        *" -target "*|*" --target "*|*" -target="*|*" --target="*)
            if ! grep -q -- 'custom crt entry compiler argv: .*target' \
                "$OUTBIN.stderr"; then
                echo "FAIL: custom entry compile did not forward target flag" >&2
                cat "$OUTBIN.stderr" >&2
                exit 1
            fi
            ;;
    esac
    case " $NCC_CRT_TEST_LDFLAGS " in
        *" -isysroot "*|*" --sysroot "*|*" -isysroot="*|*" --sysroot="*)
            if ! grep -q -- 'custom crt entry compiler argv: .*sysroot' \
                "$OUTBIN.stderr"; then
                echo "FAIL: custom entry compile did not forward sysroot flag" >&2
                cat "$OUTBIN.stderr" >&2
                exit 1
            fi
            ;;
    esac
}

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

find_llvm_objdump() {
    if [ -n "$NCC_LLVM_OBJDUMP" ]; then
        printf '%s\n' "$NCC_LLVM_OBJDUMP"
        return 0
    fi
    if command -v llvm-objdump >/dev/null 2>&1; then
        command -v llvm-objdump
        return 0
    fi
    if command -v objdump >/dev/null 2>&1; then
        command -v objdump
        return 0
    fi
    echo "FAIL: llvm-objdump or objdump is required" >&2
    exit 1
}

dump_object_headers() {
    OBJDUMP=$(find_llvm_objdump)
    if ! "$OBJDUMP" -h "$OUTBIN.o" > "$OUTBIN.dump" 2> "$OUTBIN.stderr"; then
        echo "FAIL: objdump could not read object headers" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
}

dump_object_contents() {
    OBJDUMP=$(find_llvm_objdump)
    if ! "$OBJDUMP" -s "$OUTBIN.o" > "$OUTBIN.contents" 2> "$OUTBIN.stderr"; then
        echo "FAIL: objdump could not read object contents" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
}

assert_object_hex_contains() {
    EXPECTED_HEX="$1"
    dump_object_contents
    HEX=$(LC_ALL=C tr -cd '0-9A-Fa-f' < "$OUTBIN.contents")
    if ! printf '%s' "$HEX" | grep -i -q "$EXPECTED_HEX"; then
        echo "FAIL: object contents did not contain expected hex:" \
             "$EXPECTED_HEX" >&2
        cat "$OUTBIN.contents" >&2
        exit 1
    fi
}

assert_object_dump_contains() {
    EXPECTED="$1"
    dump_object_contents
    if ! grep -i -q "$EXPECTED" "$OUTBIN.contents"; then
        echo "FAIL: object contents did not contain expected dump text:" \
             "$EXPECTED" >&2
        cat "$OUTBIN.contents" >&2
        exit 1
    fi
}

assert_comptime_section_present() {
    dump_object_headers
    if ! grep -E -q '(__n00b_ct|\.n00b\.comptime|\.n00bct)' "$OUTBIN.dump"; then
        echo "FAIL: comptime metadata section missing from object" >&2
        cat "$OUTBIN.dump" >&2
        exit 1
    fi
    strings "$OUTBIN.o" > "$OUTBIN.strings"
    if ! grep -F -q 'N0CT' "$OUTBIN.strings"; then
        echo "FAIL: comptime metadata magic missing from object" >&2
        cat "$OUTBIN.strings" >&2
        exit 1
    fi
}

assert_comptime_section_absent() {
    dump_object_headers
    if grep -E -q '(__n00b_ct|\.n00b\.comptime|\.n00bct)' "$OUTBIN.dump"; then
        echo "FAIL: comptime metadata section unexpectedly present" >&2
        cat "$OUTBIN.dump" >&2
        exit 1
    fi
    strings "$OUTBIN.o" > "$OUTBIN.strings"
    if grep -F -q 'N0CT' "$OUTBIN.strings"; then
        echo "FAIL: comptime metadata magic unexpectedly present" >&2
        cat "$OUTBIN.strings" >&2
        exit 1
    fi
}

assert_comptime_var_record_present() {
    assert_comptime_section_present
    # VAR kind=2, payload len=18, typehash("int")=0x6da88c34ba124c41
    # encoded little-endian, external linkage=1, flags=0, name_len=6, "answer".
    assert_object_dump_contains '0200 1200414c 12ba348c'
    assert_object_dump_contains 'a86d0100 0600616e 73776572'
}

assert_comptime_optional_main_record_present() {
    assert_comptime_section_present
    # Magic "N0CT", version=4, COMPTIME_MAIN kind=1, len=4,
    # normalized 3-arg signature, flags=NCC_CT_MAIN_FLAG_OPTIONAL.
    assert_object_hex_contains '4e30435404000100040003010101'
}

compile_comptime_test_runtime() {
    CC_BIN="${CC:-clang}"
    cat > "$OUTBIN.rt.c" <<'EOF'
#include <stdlib.h>
void n00b_crt_run_init_array(void) {}
void n00b_init_simple(int argc, char **argv) { (void)argc; (void)argv; }
void n00b_init_core_simple(int argc, char **argv) { (void)argc; (void)argv; }
void n00b_init_late(void) {}
void *n00b_crt_apply_comptime_image(void) { return 0; }
int n00b_run_degraded_static_inits(void) { return 0; }
[[noreturn]] void n00b_exit(int rc) { _Exit(rc); }
EOF
    cat > "$OUTBIN.start.S" <<'EOF'
#if defined(__APPLE__) && defined(__aarch64__)
    .text
    .globl _n00b_start
    .p2align 2
_n00b_start:
    mov x29, xzr
    mov x30, xzr
    bl _n00b_crt_main
    brk #0
#elif defined(__APPLE__) && defined(__x86_64__)
    .text
    .globl _n00b_start
    .p2align 4, 0x90
_n00b_start:
    xorl %ebp, %ebp
    andq $-16, %rsp
    callq _n00b_crt_main
    ud2
#elif defined(__linux__) && defined(__aarch64__)
    .text
    .globl n00b_start
    .type n00b_start,%function
n00b_start:
    mov x29, xzr
    mov x30, xzr
    ldr x0, [sp]
    add x1, sp, #8
    add x2, x1, x0, lsl #3
    add x2, x2, #8
    bl n00b_crt_main
    brk #0
    .size n00b_start, .-n00b_start
    .section .note.GNU-stack,"",@progbits
#elif defined(__linux__) && defined(__x86_64__)
    .text
    .globl n00b_start
    .type n00b_start,@function
n00b_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 16(%rsp,%rdi,8), %rdx
    andq $-16, %rsp
    call n00b_crt_main
    ud2
    .size n00b_start, .-n00b_start
    .section .note.GNU-stack,"",@progbits
#else
#error unsupported host for comptime smoke start stub
#endif
EOF
    "$CC_BIN" -c "$OUTBIN.rt.c" -o "$OUTBIN.rt.o"
    "$CC_BIN" -c -x assembler-with-cpp "$OUTBIN.start.S" \
        -o "$OUTBIN.start.o"
}

skip_test() {
    echo "SKIP: $*" >&2
    exit 77
}

n00b_runtime_root_is_staged() {
    root="$1"
    [ -n "$root" ] || return 1
    [ -f "$root/include/n00b.h" ] || return 1

    build=$(n00b_runtime_build "$root")
    [ -f "$build/libn00b.a" ] || return 1
    [ -f "$build/build.ninja" ] || return 1
}

n00b_runtime_root_try() {
    root="$1"
    if n00b_runtime_root_is_staged "$root"; then
        printf '%s\n' "$root"
        return 0
    fi
    return 1
}

n00b_runtime_root_try_tree() {
    base="$1"
    n00b_runtime_root_try "$base/.workspaces/wax-tui" && return 0

    n00b_runtime_root_try "$base" && return 0

    if [ -d "$base/.workspaces" ]; then
        for root in "$base/.workspaces"/*; do
            [ -d "$root" ] || continue
            n00b_runtime_root_try "$root" && return 0
        done
    fi

    return 1
}

n00b_runtime_root() {
    for root in "$NCC_TEST_N00B_ROOT" "$N00B_WP005_ROOT" "$N00B_WP003_ROOT"; do
        [ -n "$root" ] || continue
        n00b_runtime_root_try "$root" && return 0
        echo "FAIL: configured n00b runtime root is not staged: $root" >&2
        exit 1
    done

    n00b_runtime_root_try "$NCC_SOURCE_ROOT/subprojects/libn00b" && return 0

    search="$NCC_SOURCE_ROOT"
    while :; do
        n00b_runtime_root_try_tree "$search/n00b" && return 0
        n00b_runtime_root_try_tree "$search/../n00b" && return 0

        parent=$(dirname -- "$search")
        [ "$parent" != "$search" ] || break
        search="$parent"
    done

    if [ "${NCC_TEST_ALLOW_MISSING_N00B_RUNTIME:-0}" != "0" ] \
        || [ "${NCC_TEST_ALLOW_MISSING_N00B_WP003:-0}" != "0" ]; then
        skip_test "staged n00b runtime was not found"
    fi

    echo "FAIL: could not find a staged n00b runtime; set NCC_TEST_N00B_ROOT if it is not adjacent to ncc" >&2
    exit 1
}

n00b_runtime_build() {
    root="$1"
    if [ -n "$NCC_TEST_N00B_BUILD_DIR" ]; then
        case "$NCC_TEST_N00B_BUILD_DIR" in
            /*)
                printf '%s\n' "$NCC_TEST_N00B_BUILD_DIR"
                ;;
            *)
                printf '%s/%s\n' "$root" "$NCC_TEST_N00B_BUILD_DIR"
                ;;
        esac
        return
    fi

    printf '%s/build_debug\n' "$root"
}

n00b_test_link_args() {
    root="$1"
    build=$(n00b_runtime_build "$root")
    raw=$(awk '
        /^build test_ct_image_roundtrip:/ { found = 1; next }
        found && /^ LINK_ARGS = / {
            sub(/^ LINK_ARGS = /, "")
            print
            exit
        }
    ' "$build/build.ninja")

    if [ -z "$raw" ]; then
        return 1
    fi

    out=
    for arg in $raw; do
        case "$arg" in
            libn00b.a|subprojects/*.a)
                arg="$build/$arg"
                ;;
        esac
        out="$out $arg"
    done
    printf '%s\n' "$out"
}

n00b_host_static_object_attr() {
    case "$(uname -s)" in
        Darwin)
            printf '%s\n' '[[gnu::section("__DATA,n00b_stobj"), gnu::used]]'
            ;;
        Linux)
            printf '%s\n' '[[gnu::section("n00b_stobj"), gnu::used, gnu::retain]]'
            ;;
        *)
            return 1
            ;;
    esac
}

n00b_host_start_stub_src() {
    root="$1"
    os=$(uname -s)
    arch=$(uname -m)
    case "$os:$arch" in
        Darwin:arm64|Darwin:aarch64)
            printf '%s\n' "$root/src/crt/n00b_start_macos_arm64.S"
            ;;
        Darwin:x86_64)
            printf '%s\n' "$root/src/crt/n00b_start_macos_x64.S"
            ;;
        Linux:arm64|Linux:aarch64)
            printf '%s\n' "$root/src/crt/n00b_start_linux_arm64.S"
            ;;
        Linux:x86_64)
            printf '%s\n' "$root/src/crt/n00b_start_linux_x64.S"
            ;;
        *)
            return 1
            ;;
    esac
}

compile_n00b_host_start_stub() {
    root="$1"
    stub_src=$(n00b_host_start_stub_src "$root") \
        || skip_test "unsupported host for staged n00b crt stub"
    if [ ! -f "$stub_src" ]; then
        echo "FAIL: staged n00b crt stub source not available at $stub_src" >&2
        exit 1
    fi

    CC_BIN="${CC:-clang}"
    "$CC_BIN" -c -x assembler-with-cpp "$stub_src" -o "$OUTBIN.start.o" \
        > "$OUTBIN.stdout" 2> "$OUTBIN.stderr" || {
            echo "FAIL: could not compile staged n00b crt stub" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        }
    printf '%s\n' "$OUTBIN.start.o"
}

run_comptime_image_e2e() {
    root=$(n00b_runtime_root)
    build=$(n00b_runtime_build "$root")

    if [ ! -f "$root/include/n00b.h" ] || [ ! -f "$build/libn00b.a" ] \
        || [ ! -f "$build/build.ninja" ]; then
        if [ "${NCC_TEST_ALLOW_MISSING_N00B_RUNTIME:-0}" != "0" ] \
            || [ "${NCC_TEST_ALLOW_MISSING_N00B_WP003:-0}" != "0" ]; then
            skip_test "staged n00b runtime build artifacts not available at $root"
        fi
        echo "FAIL: staged n00b runtime build artifacts not available at $root" >&2
        exit 1
    fi
    start_obj=$(compile_n00b_host_start_stub "$root")

    static_attr=$(n00b_host_static_object_attr) \
        || skip_test "unsupported host for staged n00b e2e"
    link_args=$(n00b_test_link_args "$root") \
        || {
            echo "FAIL: could not derive staged n00b link args from $build/build.ninja" >&2
            exit 1
        }

    "$NCC" \
        --ncc-vargs-type=n00b_vargs_t \
        --ncc-rstr-string-type=n00b_string_t \
        --ncc-rstr-text-style-type=n00b_text_style_t \
        --ncc-rstr-style-record-type=n00b_style_record_t \
        --ncc-static-object-entry-attr="$static_attr" \
        '--ncc-rstr-template-plain=({static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=nullptr};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const n00b_static_object_desc_t * const $7 $13=&$6;&$0;})' \
        '--ncc-rstr-template-styled=({$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const n00b_static_object_desc_t * const $9 $15=&$8;&$1;})' \
        '--ncc-rstr-static-ref-template-plain=static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=nullptr};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const n00b_static_object_desc_t * const $7 $13=&$6;' \
        '--ncc-rstr-static-ref-template-styled=$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const n00b_static_object_desc_t * const $9 $15=&$8;' \
        '--ncc-rstr-static-ref-expr-plain=&$0' \
        '--ncc-rstr-static-ref-expr-styled=&$1' \
        -I"$root/include" \
        -I"$root/include/internal" \
        -I"$build" \
        -I"$root" \
        -I"$root/subprojects/picoquic/picoquic" \
        -I"$root/subprojects/picoquic/loglib" \
        -I"$root/subprojects/picotls/include" \
        -I"$root/subprojects/picotls/deps/cifra/src/ext" \
        -I"$root/subprojects/picotls/deps/cifra/src" \
        -I"$root/subprojects/picotls/deps/micro-ecc" \
        -I"$root/subprojects/monocypher/include" \
        -Wextra \
        -std=c23 \
        -fno-omit-frame-pointer \
        -ffunction-sections \
        -DXXH_NO_INTRINSICS=1 \
        "$@" \
        -o "$OUTBIN" "$SRC" "$start_obj" $link_args \
        > "$OUTBIN.stdout" 2> "$OUTBIN.stderr" || {
            cat "$OUTBIN.stderr" >&2
            exit 1
        }

    "$OUTBIN"
    assert_binary_comptime_section_absent
    assert_binary_comptime_image_present
}

run_static_init_mutation_error() {
    root=$(n00b_runtime_root)
    build=$(n00b_runtime_build "$root")

    if [ ! -f "$root/include/n00b.h" ] || [ ! -f "$build/libn00b.a" ] \
        || [ ! -f "$build/build.ninja" ]; then
        if [ "${NCC_TEST_ALLOW_MISSING_N00B_RUNTIME:-0}" != "0" ] \
            || [ "${NCC_TEST_ALLOW_MISSING_N00B_WP003:-0}" != "0" ]; then
            skip_test "staged n00b runtime build artifacts not available at $root"
        fi
        echo "FAIL: staged n00b runtime build artifacts not available at $root" >&2
        exit 1
    fi
    start_obj=$(compile_n00b_host_start_stub "$root")

    static_attr=$(n00b_host_static_object_attr) \
        || skip_test "unsupported host for staged n00b e2e"
    link_args=$(n00b_test_link_args "$root") \
        || {
            echo "FAIL: could not derive staged n00b link args from $build/build.ninja" >&2
            exit 1
        }

    if "$NCC" \
        --ncc-vargs-type=n00b_vargs_t \
        --ncc-rstr-string-type=n00b_string_t \
        --ncc-rstr-text-style-type=n00b_text_style_t \
        --ncc-rstr-style-record-type=n00b_style_record_t \
        --ncc-static-object-entry-attr="$static_attr" \
        '--ncc-rstr-template-plain=({static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=nullptr};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const n00b_static_object_desc_t * const $7 $13=&$6;&$0;})' \
        '--ncc-rstr-template-styled=({$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const n00b_static_object_desc_t * const $9 $15=&$8;&$1;})' \
        '--ncc-rstr-static-ref-template-plain=static n00b_string_t $0={.u8_bytes=$1,.data=$2,.codepoints=$3,.styling=nullptr};$14 static const n00b_static_object_desc_t $6={.start=(const void*)&$0,.len=(uint64_t)sizeof($0),.tinfo=$4,.scan_kind=$10,.scan_cb=$11,.scan_user=$12,.object_id=$8,.file=__FILE__,.identity=$15,.flags=$9,.cached_hash=(n00b_uint128_t)$16};static const n00b_static_object_desc_t * const $7 $13=&$6;' \
        '--ncc-rstr-static-ref-template-styled=$0 static n00b_string_t $1={.u8_bytes=$2,.data=$3,.codepoints=$4,.styling=$5};$16 static const n00b_static_object_desc_t $8={.start=(const void*)&$1,.len=(uint64_t)sizeof($1),.tinfo=$6,.scan_kind=$12,.scan_cb=$13,.scan_user=$14,.object_id=$10,.file=__FILE__,.identity=$17,.flags=$11,.cached_hash=(n00b_uint128_t)$18};static const n00b_static_object_desc_t * const $9 $15=&$8;' \
        '--ncc-rstr-static-ref-expr-plain=&$0' \
        '--ncc-rstr-static-ref-expr-styled=&$1' \
        -I"$root/include" \
        -I"$root/include/internal" \
        -I"$build" \
        -I"$root" \
        -I"$root/subprojects/picoquic/picoquic" \
        -I"$root/subprojects/picoquic/loglib" \
        -I"$root/subprojects/picotls/include" \
        -I"$root/subprojects/picotls/deps/cifra/src/ext" \
        -I"$root/subprojects/picotls/deps/cifra/src" \
        -I"$root/subprojects/picotls/deps/micro-ecc" \
        -I"$root/subprojects/monocypher/include" \
        -Wextra \
        -std=c23 \
        -fno-omit-frame-pointer \
        -ffunction-sections \
        -DXXH_NO_INTRINSICS=1 \
        -o "$OUTBIN" "$SRC" "$start_obj" $link_args \
        > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"; then
        echo "FAIL: static-init mutation build unexpectedly succeeded" >&2
        exit 1
    fi

    if ! grep -F -q "D-024: static initializer 'state' mutated during comptime run" \
        "$OUTBIN.stderr"; then
        echo "FAIL: static-init mutation error did not mention D-024" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
    if [ -e "$OUTBIN" ]; then
        echo "FAIL: static-init mutation build published an output binary" >&2
        exit 1
    fi
}

run_static_init_no_comptime_degrade() {
    root=$(n00b_runtime_root)
    build=$(n00b_runtime_build "$root")

    if [ ! -f "$root/include/n00b.h" ] || [ ! -f "$build/libn00b.a" ] \
        || [ ! -f "$build/build.ninja" ]; then
        if [ "${NCC_TEST_ALLOW_MISSING_N00B_RUNTIME:-0}" != "0" ] \
            || [ "${NCC_TEST_ALLOW_MISSING_N00B_WP003:-0}" != "0" ]; then
            skip_test "staged n00b runtime build artifacts not available at $root"
        fi
        echo "FAIL: staged n00b runtime build artifacts not available at $root" >&2
        exit 1
    fi
    start_obj=$(compile_n00b_host_start_stub "$root")

    static_attr=$(n00b_host_static_object_attr) \
        || skip_test "unsupported host for staged n00b e2e"
    link_args=$(n00b_test_link_args "$root") \
        || {
            echo "FAIL: could not derive staged n00b link args from $build/build.ninja" >&2
            exit 1
        }

    "$NCC" \
        --ncc-no-comptime \
        --ncc-vargs-type=n00b_vargs_t \
        --ncc-rstr-string-type=n00b_string_t \
        --ncc-rstr-text-style-type=n00b_text_style_t \
        --ncc-rstr-style-record-type=n00b_style_record_t \
        --ncc-static-object-entry-attr="$static_attr" \
        -I"$root/include" -I"$root/include/internal" -I"$build" -I"$root" \
        -I"$root/subprojects/picoquic/picoquic" \
        -I"$root/subprojects/picoquic/loglib" \
        -I"$root/subprojects/picotls/include" \
        -I"$root/subprojects/picotls/deps/cifra/src/ext" \
        -I"$root/subprojects/picotls/deps/cifra/src" \
        -I"$root/subprojects/picotls/deps/micro-ecc" \
        -I"$root/subprojects/monocypher/include" \
        -Wextra -std=c23 -fno-omit-frame-pointer -ffunction-sections \
        -D_GNU_SOURCE \
        -DXXH_NO_INTRINSICS=1 \
        -o "$OUTBIN" "$SRC" "$start_obj" $link_args \
        > "$OUTBIN.stdout" 2> "$OUTBIN.stderr" || {
            cat "$OUTBIN.stderr" >&2
            exit 1
        }
    "$OUTBIN"
    assert_binary_comptime_section_absent

    "$NCC" \
        --ncc-vargs-type=n00b_vargs_t \
        --ncc-rstr-string-type=n00b_string_t \
        --ncc-rstr-text-style-type=n00b_text_style_t \
        --ncc-rstr-style-record-type=n00b_style_record_t \
        --ncc-static-object-entry-attr="$static_attr" \
        -I"$root/include" -I"$root/include/internal" -I"$build" -I"$root" \
        -I"$root/subprojects/picoquic/picoquic" \
        -I"$root/subprojects/picoquic/loglib" \
        -I"$root/subprojects/picotls/include" \
        -I"$root/subprojects/picotls/deps/cifra/src/ext" \
        -I"$root/subprojects/picotls/deps/cifra/src" \
        -I"$root/subprojects/picotls/deps/micro-ecc" \
        -I"$root/subprojects/monocypher/include" \
        -Wextra -std=c23 -fno-omit-frame-pointer -ffunction-sections \
        -D_GNU_SOURCE \
        -DXXH_NO_INTRINSICS=1 \
        -c "$SRC" -o "$OUTBIN.user.o" \
        > "$OUTBIN.stdout" 2> "$OUTBIN.stderr" || {
            cat "$OUTBIN.stderr" >&2
            exit 1
        }

    "$NCC" --ncc-no-comptime -o "$OUTBIN" "$OUTBIN.user.o" \
        "$start_obj" $link_args > "$OUTBIN.stdout" 2> "$OUTBIN.stderr" || {
            cat "$OUTBIN.stderr" >&2
            exit 1
        }
    "$OUTBIN"
    assert_binary_comptime_section_absent
}

assert_binary_comptime_section_absent() {
    OBJDUMP=$(find_llvm_objdump)
    if ! "$OBJDUMP" -h "$OUTBIN" > "$OUTBIN.dump" 2> "$OUTBIN.stderr"; then
        echo "FAIL: objdump could not read linked binary headers" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
    if grep -E -q '(^|[[:space:]])(__n00b_ct|\.n00b\.comptime|\.n00bct)([[:space:]]|$)' "$OUTBIN.dump"; then
        echo "FAIL: final binary still carries comptime metadata section" >&2
        cat "$OUTBIN.dump" >&2
        exit 1
    fi
    strings "$OUTBIN" > "$OUTBIN.strings"
    if grep -F -q 'N0CT' "$OUTBIN.strings"; then
        echo "FAIL: final binary still carries comptime metadata magic" >&2
        cat "$OUTBIN.strings" >&2
        exit 1
    fi
}

assert_binary_comptime_image_present() {
    OBJDUMP=$(find_llvm_objdump)
    if ! "$OBJDUMP" -h "$OUTBIN" > "$OUTBIN.dump" 2> "$OUTBIN.stderr"; then
        echo "FAIL: objdump could not read linked binary headers" >&2
        cat "$OUTBIN.stderr" >&2
        exit 1
    fi
    if ! grep -E -q '(__n00b_ctimg|__n00b_ctwimg|\.n00b\.ctimage|\.n00b\.ctwimg|\.n00bcti|\.n00bctw)' "$OUTBIN.dump"; then
        echo "FAIL: final binary does not carry comptime image section" >&2
        cat "$OUTBIN.dump" >&2
        exit 1
    fi
}

assert_comptime_sentinel_once() {
    if [ ! -s "$OUTBIN.sentinel" ]; then
        echo "FAIL: comptime sentinel was not written" >&2
        exit 1
    fi
    if [ "$(wc -l < "$OUTBIN.sentinel" | tr -d ' ')" != "1" ]; then
        echo "FAIL: comptime sentinel should contain exactly one line" >&2
        cat "$OUTBIN.sentinel" >&2
        exit 1
    fi
    if ! grep -F -q 'argc=3 argv2=alpha env=present envp=1 answer=42' \
        "$OUTBIN.sentinel"; then
        echo "FAIL: comptime argv/env sentinel contents wrong" >&2
        cat "$OUTBIN.sentinel" >&2
        exit 1
    fi
}

non_host_comptime_target() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os:$arch" in
        Darwin:*)
            printf '%s\n' 'x86_64-unknown-linux-gnu'
            ;;
        Linux:x86_64)
            printf '%s\n' 'aarch64-unknown-linux-gnu'
            ;;
        Linux:aarch64|Linux:arm64)
            printf '%s\n' 'x86_64-unknown-linux-gnu'
            ;;
        *)
            skip_test "unsupported host for comptime cross-target guard"
            ;;
    esac
}

case "$MODE" in
    compile_run)
        "$NCC" "$@" -o "$OUTBIN" "$SRC"
        "$OUTBIN"
        ;;

    comptime_section_present)
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.o"
        assert_comptime_section_present
        ;;

    comptime_section_absent)
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.o"
        assert_comptime_section_absent
        ;;

    comptime_var_record_present)
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.o"
        assert_comptime_var_record_present
        ;;

    comptime_smoke)
        compile_comptime_test_runtime
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            --ncc-comptime-arg=alpha \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_comptime_sentinel_once
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_object_link)
        compile_comptime_test_runtime
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.user.o"
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            --ncc-comptime-arg=alpha \
            -o "$OUTBIN" "$OUTBIN.user.o" "$OUTBIN.rt.o" \
            "$OUTBIN.start.o" > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_comptime_sentinel_once
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_mixed_link)
        compile_comptime_test_runtime
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.user.o"
        cat > "$OUTBIN.c" <<'EOF'
int ncc_comptime_mixed_helper(void) { return 1; }
EOF
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            --ncc-comptime-arg=alpha \
            -o "$OUTBIN" "$OUTBIN.c" "$OUTBIN.user.o" \
            "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_comptime_sentinel_once
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_link_order)
        compile_comptime_test_runtime
        cat > "$OUTBIN.dep.c" <<'EOF'
int ncc_comptime_dep_value(void) { return 9; }
EOF
        "$CC_BIN" -c "$OUTBIN.dep.c" -o "$OUTBIN.dep.o"
        "${AR:-ar}" rcs "$OUTBIN.libdep.a" "$OUTBIN.dep.o"
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            --ncc-comptime-arg=alpha \
            -o "$OUTBIN" "$SRC" "$OUTBIN.libdep.a" \
            "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_comptime_sentinel_once
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_forced_include)
        compile_comptime_test_runtime
        cat > "$OUTBIN.force.h" <<'EOF'
enum { ncc_comptime_forced_include_once = 1 };
EOF
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            -include "$OUTBIN.force.h" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            --ncc-comptime-arg=alpha \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_comptime_sentinel_once
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_fail)
        compile_comptime_test_runtime
        if NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"; then
            echo "FAIL: expected comptime build to fail" >&2
            exit 1
        fi
        if [ -e "$OUTBIN" ]; then
            echo "FAIL: failing comptime build left final binary behind" >&2
            exit 1
        fi
        if ! grep -F -q 'comptime exited with status 7' \
            "$OUTBIN.stderr"; then
            echo "FAIL: missing comptime failure diagnostic" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    comptime_atomic_exit)
        compile_comptime_test_runtime
        printf '%s\n' 'PREEXISTING-COMPTIME-OUTPUT' > "$OUTBIN"
        if NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            --ncc-comptime-arg="$OUTBIN.sentinel" \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"; then
            echo "FAIL: expected comptime build to fail" >&2
            exit 1
        fi
        if ! grep -F -q 'comptime exited with status 7' \
            "$OUTBIN.stderr"; then
            echo "FAIL: missing comptime exit diagnostic" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -F -x -q 'PREEXISTING-COMPTIME-OUTPUT' "$OUTBIN"; then
            echo "FAIL: failing comptime build clobbered prior output" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    comptime_atomic_signal)
        compile_comptime_test_runtime
        printf '%s\n' 'PREEXISTING-COMPTIME-OUTPUT' > "$OUTBIN"
        if NCC_CT_PHASE5_ENV=present "$NCC" "$@" \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"; then
            echo "FAIL: expected comptime signal build to fail" >&2
            exit 1
        fi
        case "$(uname -s 2>/dev/null || printf unknown)" in
            MINGW*|MSYS*|CYGWIN*|Windows_NT)
                EXPECTED_CRASH='comptime execution crashed: exception'
                ;;
            *)
                EXPECTED_CRASH='comptime execution crashed: signal'
                ;;
        esac
        if ! grep -F -q "$EXPECTED_CRASH" "$OUTBIN.stderr"; then
            echo "FAIL: missing comptime crash diagnostic: $EXPECTED_CRASH" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -F -x -q 'PREEXISTING-COMPTIME-OUTPUT' "$OUTBIN"; then
            echo "FAIL: signaled comptime build clobbered prior output" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    comptime_no_comptime)
        compile_comptime_test_runtime
        NCC_CT_PHASE5_ENV=present "$NCC" "$@" --ncc-no-comptime \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        NCC_CT_PHASE5_ENV=present "$OUTBIN" "$OUTBIN.sentinel" alpha
        assert_comptime_sentinel_once
        assert_binary_comptime_section_absent
        ;;

    comptime_optional_metadata)
        compile_comptime_test_runtime
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.o"
        assert_comptime_optional_main_record_present
        "$NCC" "$@" --ncc-no-comptime \
            -o "$OUTBIN" "$OUTBIN.o" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN" "$OUTBIN.sentinel"
        if ! grep -F -q 'answer=77' "$OUTBIN.sentinel"; then
            echo "FAIL: optional object metadata did not select runtime degrade" >&2
            cat "$OUTBIN.sentinel" >&2
            exit 1
        fi
        assert_binary_comptime_section_absent
        ;;

    comptime_degrade)
        compile_comptime_test_runtime
        "$NCC" "$@" --ncc-no-comptime \
            -o "$OUTBIN" "$SRC" "$OUTBIN.rt.o" "$OUTBIN.start.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN" "$OUTBIN.sentinel"
        if ! grep -F -q 'answer=77' "$OUTBIN.sentinel"; then
            echo "FAIL: source+link degrade did not initialize comptime var" >&2
            cat "$OUTBIN.sentinel" >&2
            exit 1
        fi
        assert_binary_comptime_section_absent

        rm -f "$OUTBIN.sentinel"
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.user.o"
        "$NCC" "$@" --ncc-no-comptime \
            -o "$OUTBIN" "$OUTBIN.user.o" "$OUTBIN.rt.o" \
            "$OUTBIN.start.o" > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN" "$OUTBIN.sentinel"
        if ! grep -F -q 'answer=77' "$OUTBIN.sentinel"; then
            echo "FAIL: object-only degrade did not initialize comptime var" >&2
            cat "$OUTBIN.sentinel" >&2
            exit 1
        fi
        assert_binary_comptime_section_absent
        ;;

    comptime_xcompile_guard)
        TARGET=$(non_host_comptime_target)
        set +e
        "$NCC" "$@" --target="$TARGET" -o "$OUTBIN" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        STATUS=$?
        set -e
        if [ "$STATUS" -eq 0 ]; then
            echo "FAIL: expected cross-target comptime guard failure" >&2
            exit 1
        fi
        if ! grep -F -q 'cannot run comptime due to platform mismatch' \
            "$OUTBIN.stderr"; then
            echo "FAIL: missing cross-target comptime guard diagnostic" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if [ -e "$OUTBIN" ]; then
            echo "FAIL: cross-target comptime guard produced requested output" >&2
            ls -l "$OUTBIN" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    comptime_var_link_strip)
        "$NCC" "$@" -o "$OUTBIN" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_var_object_link_strip)
        "$NCC" "$@" -c "$SRC" -o "$OUTBIN.user.o"
        "$NCC" "$@" -o "$OUTBIN" "$OUTBIN.user.o" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN"
        assert_binary_comptime_section_absent
        ;;

    comptime_image_e2e)
        run_comptime_image_e2e
        ;;

    static_init_mutation_error)
        run_static_init_mutation_error
        ;;

    static_init_no_comptime_degrade)
        run_static_init_no_comptime_degrade
        ;;

    preprocess)
        OUTPUT=$("$NCC" "$@" -E "$SRC")
        if [ -z "$OUTPUT" ]; then
            echo "FAIL: -E produced empty output" >&2
            exit 1
        fi
        ;;

    preprocess_contains)
        EXPECTED="$1"
        shift || true
        if [ -z "$EXPECTED" ]; then
            echo "FAIL: expected output substring is required" >&2
            exit 1
        fi
        "$NCC" "$@" -E "$SRC" > "$OUTBIN.c"
        if ! grep -F -q "$EXPECTED" "$OUTBIN.c"; then
            echo "FAIL: transformed output did not contain expected text:" \
                 "$EXPECTED" >&2
            cat "$OUTBIN.c" >&2
            exit 1
        fi
        ;;

    preprocess_not_contains)
        UNEXPECTED="$1"
        shift || true
        if [ -z "$UNEXPECTED" ]; then
            echo "FAIL: unexpected output substring is required" >&2
            exit 1
        fi
        "$NCC" "$@" -E "$SRC" > "$OUTBIN.c"
        if grep -F -q "$UNEXPECTED" "$OUTBIN.c"; then
            echo "FAIL: transformed output contained unexpected text:" \
                 "$UNEXPECTED" >&2
            cat "$OUTBIN.c" >&2
            exit 1
        fi
        ;;

    preprocess_stderr_contains)
        # Asserts on `ncc -E` paths that exit 0 (no error) AND produce
        # a stderr diagnostic AND omit a substring from stdout. Used for
        # warn-and-skip diagnostics (D-009 / Phase 5 incomplete-struct
        # warning) where the build is supposed to succeed but a specific
        # warning must appear and the decl must NOT be auto-registered.
        EXPECTED_STDERR="$1"
        shift || true
        UNEXPECTED_STDOUT="$1"
        shift || true
        if [ -z "$EXPECTED_STDERR" ] || [ -z "$UNEXPECTED_STDOUT" ]; then
            echo "FAIL: preprocess_stderr_contains needs <stderr-substring>" \
                 "<stdout-omit-substring>" >&2
            exit 1
        fi
        "$NCC" "$@" -E "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if ! grep -F -q "$EXPECTED_STDERR" "$OUTBIN.stderr"; then
            echo "FAIL: stderr did not contain expected text:" \
                 "$EXPECTED_STDERR" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if grep -F -q "$UNEXPECTED_STDOUT" "$OUTBIN.stdout"; then
            echo "FAIL: stdout contained text that should have been skipped:" \
                 "$UNEXPECTED_STDOUT" >&2
            cat "$OUTBIN.stdout" >&2
            exit 1
        fi
        ;;

    preprocess_stderr_omits)
        # Run with -E; expect exit 0 (enforced by `set -e`) AND a specific
        # substring ABSENT from stderr — for safe patterns that must NOT
        # produce a diagnostic (e.g. a properly-guarded nullable deref).
        UNEXPECTED_STDERR="$1"
        shift || true
        if [ -z "$UNEXPECTED_STDERR" ]; then
            echo "FAIL: preprocess_stderr_omits needs <stderr-omit-substring>" \
                 >&2
            exit 1
        fi
        "$NCC" "$@" -E "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if grep -F -q "$UNEXPECTED_STDERR" "$OUTBIN.stderr"; then
            echo "FAIL: stderr contained text that should be absent:" \
                 "$UNEXPECTED_STDERR" >&2
            cat "$OUTBIN.stderr" >&2
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

    expect_error_contains)
        EXPECTED="$1"
        shift || true
        if [ -z "$EXPECTED" ]; then
            echo "FAIL: expected diagnostic substring is required" >&2
            exit 1
        fi
        set +e
        "$NCC" "$@" -o "$OUTBIN" "$SRC" 2> "$OUTBIN.stderr"
        STATUS=$?
        set -e
        if [ "$STATUS" -eq 0 ]; then
            echo "FAIL: expected non-zero exit from ncc" >&2
            exit 1
        fi
        if ! grep -F -q "$EXPECTED" "$OUTBIN.stderr"; then
            echo "FAIL: diagnostic did not contain expected text:" \
                 "$EXPECTED" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    expect_error_contains_no_output)
        EXPECTED="$1"
        shift || true
        if [ -z "$EXPECTED" ]; then
            echo "FAIL: expected diagnostic substring is required" >&2
            exit 1
        fi
        set +e
        "$NCC" "$@" -o "$OUTBIN" "$SRC" 2> "$OUTBIN.stderr"
        STATUS=$?
        set -e
        if [ "$STATUS" -eq 0 ]; then
            echo "FAIL: expected non-zero exit from ncc" >&2
            exit 1
        fi
        if ! grep -F -q "$EXPECTED" "$OUTBIN.stderr"; then
            echo "FAIL: diagnostic did not contain expected text:" \
                 "$EXPECTED" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if [ -e "$OUTBIN" ]; then
            echo "FAIL: failing ncc invocation produced requested output" >&2
            ls -l "$OUTBIN" >&2
            cat "$OUTBIN.stderr" >&2
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

    compiler_temp_source)
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
        if ! grep -q 'fake final compiler received temp source file' \
            "$OUTBIN.stderr"; then
            echo "FAIL: final compiler diagnostic was not preserved" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if grep -q 'fake final compiler expected temp source file' \
            "$OUTBIN.stderr"; then
            echo "FAIL: final compiler did not receive a temp source file" >&2
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

    custom_entry_run)
        if [ -z "$NCC_CRT_TEST_LDFLAGS" ]; then
            echo "SKIP: NCC_CRT_TEST_LDFLAGS is required for custom_entry_run" >&2
            exit 77
        fi
        # shellcheck disable=SC2086
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -o "$OUTBIN" "$SRC" \
            $NCC_CRT_TEST_LDFLAGS > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if ! grep -F -q -- '-nostartfiles' "$OUTBIN.stderr"; then
            echo "FAIL: custom entry link did not pass -nostartfiles" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -F -q -- 'n00b_start' "$OUTBIN.stderr"; then
            echo "FAIL: custom entry link did not pass an n00b_start entry flag" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        assert_custom_entry_target_forwarding
        "$OUTBIN"
        ;;

    custom_entry_object_run)
        if [ -z "$NCC_CRT_TEST_LDFLAGS" ]; then
            echo "SKIP: NCC_CRT_TEST_LDFLAGS is required for custom_entry_object_run" >&2
            exit 77
        fi
        "$NCC" "$@" -c -o "$OUTBIN.o" "$SRC"
        # shellcheck disable=SC2086
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -o "$OUTBIN" "$OUTBIN.o" \
            $NCC_CRT_TEST_LDFLAGS > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if ! grep -F -q -- 'custom-entry passthrough linker argv' "$OUTBIN.stderr"; then
            echo "FAIL: object-only custom entry link did not use passthrough linker path" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -F -q -- '-nostartfiles' "$OUTBIN.stderr"; then
            echo "FAIL: object-only custom entry link did not pass -nostartfiles" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if ! grep -F -q -- 'n00b_start' "$OUTBIN.stderr"; then
            echo "FAIL: object-only custom entry link did not pass an n00b_start entry flag" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        assert_custom_entry_target_forwarding
        "$OUTBIN"
        ;;

    custom_entry_default_no_inject)
        NCC_VERBOSE=1 "$NCC" "$@" -o "$OUTBIN" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        "$OUTBIN"
        if grep -F -q -- '-nostartfiles' "$OUTBIN.stderr"; then
            echo "FAIL: default link unexpectedly passed -nostartfiles" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        if grep -F -q -- 'custom crt entry compiler argv' "$OUTBIN.stderr"; then
            echo "FAIL: default link unexpectedly compiled generated crt entry" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        ;;

    custom_entry_compile_only_no_inject)
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -c -o "$OUTBIN.o" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if [ ! -s "$OUTBIN.o" ]; then
            echo "FAIL: expected compile-only object output" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        assert_no_custom_entry_injection
        ;;

    custom_entry_assemble_only_no_inject)
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -S -o "$OUTBIN.s" "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if [ ! -s "$OUTBIN.s" ]; then
            echo "FAIL: expected assembly output" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        assert_no_custom_entry_injection
        ;;

    custom_entry_syntax_only_no_inject)
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -fsyntax-only "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        assert_no_custom_entry_injection
        ;;

    custom_entry_dep_only_no_inject)
        NCC_VERBOSE=1 "$NCC" --ncc-custom-entry "$@" -M "$SRC" \
            > "$OUTBIN.stdout" 2> "$OUTBIN.stderr"
        if ! grep -F -q -- 'test_crt_entry_smoke.c' "$OUTBIN.stdout"; then
            echo "FAIL: expected dependency-only output" >&2
            cat "$OUTBIN.stdout" >&2
            cat "$OUTBIN.stderr" >&2
            exit 1
        fi
        assert_no_custom_entry_injection
        ;;

    *)
        echo "unknown mode: $MODE" >&2
        exit 1
        ;;
esac
