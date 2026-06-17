#!/usr/bin/env python3
"""Native Windows NCC compiler smoke test runner.

This mirrors run_test.sh for Meson builds that cannot rely on a POSIX shell.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str, detail: str | None = None) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    if detail:
        print(detail, file=sys.stderr)
    raise SystemExit(1)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def write_binary_expected(path: Path) -> None:
    path.write_bytes(b"A\x00B")


def same_bytes(left: Path, right: Path) -> bool:
    return left.read_bytes() == right.read_bytes()


def run_cmd(
    argv: list[str | Path],
    *,
    stdout_path: Path | None = None,
    stderr_path: Path | None = None,
    stdout_devnull: bool = False,
    stderr_devnull: bool = False,
    env_set: dict[str, str] | None = None,
    env_unset: list[str] | None = None,
    check: bool = True,
) -> int:
    env = os.environ.copy()
    for name in env_unset or []:
        env.pop(name, None)
    for name, value in (env_set or {}).items():
        env[name] = value

    stdout_handle = None
    stderr_handle = None
    try:
        if stdout_path is not None:
            stdout_handle = stdout_path.open("wb")
            stdout = stdout_handle
        elif stdout_devnull:
            stdout = subprocess.DEVNULL
        else:
            stdout = None

        if stderr_path is not None:
            stderr_handle = stderr_path.open("wb")
            stderr = stderr_handle
        elif stderr_devnull:
            stderr = subprocess.DEVNULL
        else:
            stderr = None

        proc = subprocess.run(
            [str(arg) for arg in argv],
            stdout=stdout,
            stderr=stderr,
            env=env,
            check=False,
        )
    finally:
        if stdout_handle is not None:
            stdout_handle.close()
        if stderr_handle is not None:
            stderr_handle.close()

    if check and proc.returncode != 0:
        fail(f"command failed with exit code {proc.returncode}: {argv[0]}")
    return proc.returncode


def check_contains(path: Path, needle: str, message: str) -> None:
    data = read_text(path)
    if needle not in data:
        fail(message, data)


def check_not_contains(path: Path, needle: str, message: str) -> None:
    data = read_text(path)
    if needle in data:
        fail(message, data)


def check_depfile_mentions_header(depfile: Path) -> None:
    if not depfile.exists() or depfile.stat().st_size == 0:
        fail("expected non-empty depfile")
    data = read_text(depfile)
    if "dep_header.h" not in data:
        fail("depfile does not name dep_header.h", data)


def depfile_first_rule(depfile: Path) -> str:
    return read_text(depfile).splitlines()[0]


def pop_arg(rest: list[str], message: str) -> str:
    if not rest:
        fail(message)
    return rest.pop(0)


def skip(message: str) -> None:
    print(f"SKIP: {message}", file=sys.stderr)
    raise SystemExit(77)


def object_headers(path: Path) -> str:
    objdump = (
        os.environ.get("NCC_LLVM_OBJDUMP")
        or shutil.which("llvm-objdump")
        or shutil.which("objdump")
    )
    if not objdump:
        fail("llvm-objdump or objdump is required")
    proc = subprocess.run(
        [objdump, "-h", str(path)],
        capture_output=True,
        text=True,
        errors="replace",
        check=False,
    )
    if proc.returncode:
        fail(f"objdump could not read {path}", proc.stderr)
    return proc.stdout


def assert_comptime_metadata(path: Path, present: bool) -> None:
    headers = object_headers(path)
    has_section = any(
        name in headers for name in ("__n00b_ct", ".n00b.comptime", ".n00bct")
    )
    has_magic = b"N0CT" in path.read_bytes()
    if present and not (has_section and has_magic):
        fail("comptime metadata section or magic missing", headers)
    if not present and (has_section or has_magic):
        fail("comptime metadata section or magic unexpectedly present", headers)


def assert_no_custom_entry_injection(stderr_file: Path) -> None:
    check_not_contains(
        stderr_file,
        "-nostartfiles",
        "no-link custom entry unexpectedly passed -nostartfiles",
    )
    check_not_contains(
        stderr_file,
        "custom crt entry compiler argv",
        "no-link custom entry unexpectedly compiled generated crt entry",
    )


# ponytail: these need a native Windows n00b_start/runtime fixture; enable them
# when that fixture exists instead of pretending the POSIX assembly stub works.
WINDOWS_UNSUPPORTED_MODES = {
    "comptime_smoke",
    "comptime_object_link",
    "comptime_mixed_link",
    "comptime_link_order",
    "comptime_forced_include",
    "comptime_fail",
    "comptime_atomic_exit",
    "comptime_atomic_signal",
    "comptime_no_comptime",
    "comptime_optional_metadata",
    "comptime_degrade",
    "comptime_var_link_strip",
    "comptime_var_object_link_strip",
    "comptime_image_e2e",
    "static_init_mutation_error",
    "static_init_no_comptime_degrade",
    "custom_entry_run",
    "custom_entry_object_run",
}


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        fail("usage: run_test.py <ncc> <mode> <source> [ncc-flags...]")

    ncc = argv[0]
    mode = argv[1]
    src = argv[2]
    rest = argv[3:]

    tmp_root = Path(os.environ.get("MESON_BUILD_ROOT") or tempfile.gettempdir())
    work = Path(tempfile.mkdtemp(prefix=f"ncc_test_{os.getpid()}_", dir=tmp_root))
    outbase = work / "ncc_test"
    outbin = outbase.with_suffix(".exe")
    outc = outbase.with_suffix(".c")
    outobj = outbase.with_suffix(".o")
    outasm = outbase.with_suffix(".s")
    outdep = outbase.with_suffix(".d")
    stdout_file = outbase.with_suffix(".stdout")
    stderr_file = outbase.with_suffix(".stderr")
    expected_file = outbase.with_suffix(".expected")

    try:
        if mode in WINDOWS_UNSUPPORTED_MODES:
            skip("native Windows runtime fixture is not available for this mode")

        if mode == "compile_run":
            run_cmd([ncc, *rest, "-o", outbin, src])
            run_cmd([outbin])

        elif mode == "compile_only":
            run_cmd([ncc, *rest, "-c", "-o", outobj, src])
            if not outobj.exists() or outobj.stat().st_size == 0:
                fail("expected non-empty object output")

        elif mode in {
            "comptime_section_present",
            "comptime_section_absent",
            "comptime_var_record_present",
        }:
            run_cmd([ncc, *rest, "-c", src, "-o", outobj])
            assert_comptime_metadata(
                outobj, present=mode != "comptime_section_absent"
            )
            if mode == "comptime_var_record_present":
                expected_record = bytes.fromhex(
                    "02001200414c12ba348ca86d01000600616e73776572"
                )
                if expected_record not in outobj.read_bytes():
                    fail("comptime VAR metadata record missing")

        elif mode == "comptime_xcompile_guard":
            status = run_cmd(
                [
                    ncc,
                    *rest,
                    "--target=x86_64-unknown-linux-gnu",
                    "-o",
                    outbin,
                    src,
                ],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("expected cross-target comptime guard failure")
            check_contains(
                stderr_file,
                "cannot run comptime due to platform mismatch",
                "missing cross-target comptime guard diagnostic",
            )
            if outbin.exists():
                fail("cross-target comptime guard produced requested output")

        elif mode == "preprocess":
            run_cmd([ncc, *rest, "-E", src], stdout_path=stdout_file)
            if stdout_file.stat().st_size == 0:
                fail("-E produced empty output")

        elif mode == "preprocess_contains":
            expected = pop_arg(rest, "expected output substring is required")
            run_cmd([ncc, *rest, "-E", src], stdout_path=outc)
            check_contains(
                outc,
                expected,
                f"transformed output did not contain expected text: {expected}",
            )

        elif mode == "preprocess_not_contains":
            unexpected = pop_arg(rest, "unexpected output substring is required")
            run_cmd([ncc, *rest, "-E", src], stdout_path=outc)
            check_not_contains(
                outc,
                unexpected,
                f"transformed output contained unexpected text: {unexpected}",
            )

        elif mode == "preprocess_stderr_contains":
            expected_stderr = pop_arg(
                rest, "preprocess_stderr_contains needs <stderr-substring>"
            )
            unexpected_stdout = pop_arg(
                rest, "preprocess_stderr_contains needs <stdout-omit-substring>"
            )
            run_cmd(
                [ncc, *rest, "-E", src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
            )
            check_contains(
                stderr_file,
                expected_stderr,
                f"stderr did not contain expected text: {expected_stderr}",
            )
            check_not_contains(
                stdout_file,
                unexpected_stdout,
                f"stdout contained text that should have been skipped: {unexpected_stdout}",
            )

        elif mode == "preprocess_stderr_omits":
            unexpected_stderr = pop_arg(
                rest, "preprocess_stderr_omits needs <stderr-omit-substring>"
            )
            run_cmd(
                [ncc, *rest, "-E", src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
            )
            check_not_contains(
                stderr_file,
                unexpected_stderr,
                f"stderr contained text that should be absent: {unexpected_stderr}",
            )

        elif mode == "no_ncc":
            run_cmd([ncc, "--no-ncc", *rest, "-o", outbin, src])
            run_cmd([outbin])

        elif mode == "expect_error":
            status = run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stderr_devnull=True,
                check=False,
            )
            if status == 0:
                fail("expected non-zero exit from ncc")

        elif mode == "expect_error_contains":
            expected = pop_arg(rest, "expected diagnostic substring is required")
            status = run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("expected non-zero exit from ncc")
            check_contains(
                stderr_file,
                expected,
                f"diagnostic did not contain expected text: {expected}",
            )

        elif mode == "expect_error_contains_no_output":
            expected = pop_arg(rest, "expected diagnostic substring is required")
            status = run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("expected non-zero exit from ncc")
            check_contains(
                stderr_file,
                expected,
                f"diagnostic did not contain expected text: {expected}",
            )
            if outbin.exists():
                fail("failing ncc invocation produced requested output")

        elif mode == "depfile":
            run_cmd([ncc, *rest, "-c", "-MMD", "-MF", outdep, "-o", outobj, src])
            check_depfile_mentions_header(outdep)

        elif mode == "depfile_md":
            run_cmd([ncc, *rest, "-c", "-MD", "-MF", outdep, "-o", outobj, src])
            check_depfile_mentions_header(outdep)
            first_rule = depfile_first_rule(outdep).replace("\\", "/")
            if "ncc_test.o:" not in first_rule:
                fail("-MD depfile first rule does not target -o object", read_text(outdep))

        elif mode == "depfile_default":
            run_cmd([ncc, *rest, "-c", "-MMD", "-o", outobj, src], stdout_path=stdout_file)
            if stdout_file.exists() and stdout_file.stat().st_size > 0:
                fail("dependency rule was printed to stdout", read_text(stdout_file))
            check_depfile_mentions_header(outdep)

        elif mode == "depfile_mt":
            run_cmd(
                [ncc, *rest, "-c", "-MMD", "-MF", outdep, "-MT", "custom-target", "-o", outobj, src]
            )
            check_depfile_mentions_header(outdep)
            if not depfile_first_rule(outdep).startswith("custom-target:"):
                fail("depfile first rule does not use -MT target", read_text(outdep))

        elif mode == "depfile_mq":
            run_cmd(
                [ncc, *rest, "-c", "-MMD", "-MF", outdep, "-MQ", "space target", "-o", outobj, src]
            )
            check_depfile_mentions_header(outdep)
            if not depfile_first_rule(outdep).startswith("space\\ target:"):
                fail("depfile first rule does not use -MQ target", read_text(outdep))

        elif mode == "depfile_expect_error":
            missing_dep = str(outbase) + ".missing/out.d"
            status = run_cmd(
                [ncc, *rest, "-c", "-MMD", "-MF", missing_dep, "-o", outobj, src],
                stderr_devnull=True,
                check=False,
            )
            if status == 0:
                fail("expected depfile generation failure")
            if outobj.exists():
                fail("object file exists after depfile failure")

        elif mode == "constexpr_runtime_error":
            status = run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("expected constexpr runtime failure")
            check_contains(
                stderr_file,
                "helper execution failed",
                "diagnostic does not name helper execution phase",
            )
            check_contains(
                stderr_file,
                "exit status",
                "diagnostic does not include exit status",
            )
            check_not_contains(
                stderr_file,
                "constexpr: compilation failed",
                "runtime failure was labelled as compilation failure",
            )

        elif mode == "cc_fallback_binary_output":
            test_cc = os.environ.get("NCC_TEST_CC")
            if not test_cc:
                fail("NCC_TEST_CC is required for cc_fallback_binary_output")
            write_binary_expected(expected_file)
            run_cmd(
                [ncc, "--no-ncc", *rest, src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                env_set={"CC": test_cc, "NCC_VERBOSE": "1"},
                env_unset=["NCC_COMPILER"],
            )
            if not same_bytes(expected_file, stdout_file):
                fail("CC fallback compiler stdout bytes changed")
            check_contains(stderr_file, "using CC=", "verbose output did not show CC fallback")

        elif mode == "cc_self_guard":
            run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                env_set={"CC": ncc, "NCC_VERBOSE": "1"},
                env_unset=["NCC_COMPILER"],
            )
            run_cmd([outbin])
            check_contains(
                stderr_file,
                "ignoring CC=",
                "verbose output did not show CC self-recursion guard",
            )

        elif mode == "compiler_temp_source":
            status = run_cmd(
                [ncc, *rest, "-c", "-o", outobj, src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                env_set={"NCC_VERBOSE": "1"},
                check=False,
            )
            if status != 42:
                fail(f"expected final compiler exit 42, got {status}", read_text(stderr_file))
            check_contains(
                stderr_file,
                "fake final compiler received temp source file",
                "final compiler diagnostic was not preserved",
            )
            check_not_contains(
                stderr_file,
                "fake final compiler expected temp source file",
                "final compiler did not receive a temp source file",
            )

        elif mode == "verbose_binary_output":
            write_binary_expected(expected_file)
            run_cmd(
                [ncc, "--no-ncc", *rest, src],
                stdout_path=stdout_file,
                env_set={"NCC_VERBOSE": "1"},
            )
            if not same_bytes(expected_file, stdout_file):
                fail("verbose compiler stdout bytes changed")

        elif mode == "verbose_high_output":
            run_cmd(
                [ncc, *rest, "-c", "-o", outobj, src],
                stdout_path=stdout_file,
                env_set={"NCC_VERBOSE": "1"},
            )
            if not outobj.exists() or outobj.stat().st_size == 0:
                fail("expected fake object output")
            if not stdout_file.exists() or stdout_file.stat().st_size == 0:
                fail("expected verbose compiler stdout")

        elif mode == "custom_entry_default_no_inject":
            run_cmd(
                [ncc, *rest, "-o", outbin, src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                env_set={"NCC_VERBOSE": "1"},
            )
            run_cmd([outbin])
            check_not_contains(
                stderr_file,
                "-nostartfiles",
                "default link unexpectedly passed -nostartfiles",
            )
            check_not_contains(
                stderr_file,
                "custom crt entry compiler argv",
                "default link unexpectedly compiled generated crt entry",
            )

        elif mode in {
            "custom_entry_compile_only_no_inject",
            "custom_entry_assemble_only_no_inject",
            "custom_entry_syntax_only_no_inject",
            "custom_entry_dep_only_no_inject",
        }:
            mode_args: dict[str, list[str | Path]] = {
                "custom_entry_compile_only_no_inject": ["-c", "-o", outobj],
                "custom_entry_assemble_only_no_inject": ["-S", "-o", outasm],
                "custom_entry_syntax_only_no_inject": ["-fsyntax-only"],
                "custom_entry_dep_only_no_inject": ["-M"],
            }
            run_cmd(
                [ncc, "--ncc-custom-entry", *rest, *mode_args[mode], src],
                stdout_path=stdout_file,
                stderr_path=stderr_file,
                env_set={"NCC_VERBOSE": "1"},
            )
            if mode == "custom_entry_compile_only_no_inject" and (
                not outobj.exists() or outobj.stat().st_size == 0
            ):
                fail("expected compile-only object output")
            if mode == "custom_entry_assemble_only_no_inject" and (
                not outasm.exists() or outasm.stat().st_size == 0
            ):
                fail("expected assembly output")
            if mode == "custom_entry_dep_only_no_inject":
                check_contains(
                    stdout_file,
                    "test_crt_entry_smoke.c",
                    "expected dependency-only output",
                )
            assert_no_custom_entry_injection(stderr_file)

        else:
            fail(f"unknown mode: {mode}")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
