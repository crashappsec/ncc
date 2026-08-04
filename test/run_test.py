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


def write_response(path: Path, args: list[str | Path]) -> None:
    path.write_text(
        subprocess.list2cmdline([str(arg) for arg in args]), encoding="utf-8"
    )


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
        detail = "\n".join(
            read_text(path)
            for path in (stderr_path, stdout_path)
            if path is not None and path.stat().st_size
        )
        fail(f"command failed with exit code {proc.returncode}: {argv[0]}", detail)
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

        elif mode == "runner_failure_diagnostics":
            marker = "GROUPED005_RUNNER_STDERR_MARKER_4A12"
            status = run_cmd(
                [
                    sys.executable,
                    Path(__file__).resolve(),
                    sys.executable,
                    "preprocess_stderr_contains",
                    src,
                    marker,
                    "GROUPED005_STDOUT_SENTINEL",
                    "-c",
                    f"import sys; print({marker!r}, file=sys.stderr); raise SystemExit(23)",
                ],
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("expected nested runner failure")
            check_contains(
                stderr_file,
                marker,
                "checked command omitted redirected stderr",
            )

        elif mode == "windows_response_file":
            if os.name != "nt":
                skip("response-file normalization is Windows-specific")

            metadata_src = pop_arg(rest, "metadata source is required")
            response_cases = {
                "direct": [
                    ncc,
                    *rest,
                    "--ncc-error-on-union",
                    src,
                    "-c",
                    "-o",
                    work / "direct.obj",
                ],
                "lone": [ncc, f"@{work / 'lone.rsp'}"],
                "mixed-before": [
                    ncc,
                    f"@{work / 'mixed-before.rsp'}",
                    "--ncc-error-on-union",
                ],
                "mixed-after": [
                    ncc,
                    "--ncc-error-on-union",
                    f"@{work / 'mixed-after.rsp'}",
                ],
                "nested": [ncc, f"@{work / 'outer.rsp'}"],
            }
            write_response(
                work / "lone.rsp",
                [
                    *rest,
                    "--ncc-error-on-union",
                    src,
                    "-c",
                    "-o",
                    work / "lone.obj",
                ],
            )
            for name in ("mixed-before", "mixed-after"):
                write_response(
                    work / f"{name}.rsp",
                    [*rest, src, "-c", "-o", work / f"{name}.obj"],
                )
            write_response(
                work / "inner.rsp",
                [*rest, src, "-c", "-o", work / "nested.obj"],
            )
            write_response(
                work / "outer.rsp",
                ["--ncc-error-on-union", f"@{work / 'inner.rsp'}"],
            )

            for name, command in response_cases.items():
                diagnostic = work / f"{name}.stderr"
                status = run_cmd(command, stderr_path=diagnostic, check=False)
                if status == 0:
                    fail(f"{name} response-file form bypassed NCC union policy")
                check_contains(
                    diagnostic,
                    "traditional C union is discouraged",
                    f"{name} response-file form did not reach NCC processing",
                )

            write_response(
                work / "dump.rsp",
                [*rest, src, "-c", "-o", work / "dump-response.obj"],
            )
            for name, command in {
                "dump-direct": [
                    ncc,
                    *rest,
                    "--ncc-dump-output",
                    src,
                    "-c",
                    "-o",
                    work / "dump-direct.obj",
                ],
                "dump-response": [
                    ncc,
                    "--ncc-dump-output",
                    f"@{work / 'dump.rsp'}",
                ],
            }.items():
                diagnostic = work / f"{name}.stderr"
                run_cmd(command, stderr_path=diagnostic)
                check_contains(
                    diagnostic,
                    "=== NCC EMITTED OUTPUT ===",
                    f"{name} form omitted NCC dump output",
                )

            metadata_obj = work / "metadata.obj"
            run_cmd([ncc, *rest, "-c", metadata_src, "-o", metadata_obj])
            assert_comptime_metadata(metadata_obj, present=True)
            direct_exe = work / "metadata-direct.exe"
            response_exe = work / "metadata-response.exe"
            write_response(work / "metadata.rsp", [*rest, metadata_obj])
            for name, command in {
                "metadata-direct": [
                    ncc,
                    *rest,
                    "--ncc-no-comptime",
                    metadata_obj,
                    "-o",
                    direct_exe,
                ],
                "metadata-response": [
                    ncc,
                    "--ncc-no-comptime",
                    f"@{work / 'metadata.rsp'}",
                    "-o",
                    response_exe,
                ],
            }.items():
                diagnostic = work / f"{name}.stderr"
                run_cmd(
                    command,
                    stderr_path=diagnostic,
                    env_set={"NCC_VERBOSE": "1"},
                )
                check_contains(
                    diagnostic,
                    "--dump-section=.n00bct=",
                    f"{name} form skipped NCC metadata processing",
                )

            invalid_cases = {
                "missing": work / "missing.rsp",
                "malformed": work / "malformed.rsp",
            }
            invalid_cases["malformed"].write_text('"unterminated', encoding="utf-8")
            for i in range(17):
                write_response(
                    work / f"depth-{i}.rsp",
                    [f"@{work / f'depth-{i + 1}.rsp'}"] if i < 16 else [src],
                )
            invalid_cases["nested"] = work / "depth-0.rsp"
            expected_diagnostics = {
                "missing": "cannot read response file",
                "malformed": "unterminated quote in response file",
                "nested": "response-file nesting exceeds",
            }
            for name, response in invalid_cases.items():
                diagnostic = work / f"invalid-{name}.stderr"
                status = run_cmd(
                    [ncc, f"@{response}"], stderr_path=diagnostic, check=False
                )
                if status == 0:
                    fail(f"invalid {name} response file succeeded")
                check_contains(
                    diagnostic,
                    expected_diagnostics[name],
                    f"invalid {name} response file omitted its diagnostic",
                )

            link_case_src = work / "link-case.c"
            link_case_src.write_text(
                "int comptime_main(int argc, char **argv, char **envp) { "
                "(void)argc; (void)argv; (void)envp; return 0; }\n"
                "int main(void) { return 0; }\n",
                encoding="utf-8",
            )
            failing_case_src = work / "link-case-fail.c"
            failing_case_src.write_text(
                "int comptime_main(int argc, char **argv, char **envp) { "
                "(void)argc; (void)argv; (void)envp; return 7; }\n"
                "int main(void) { return 0; }\n",
                encoding="utf-8",
            )
            runtime_src = work / "runtime.c"
            runtime_src.write_text(
                "__declspec(dllimport) void __stdcall ExitProcess(unsigned int);\n"
                "[[noreturn]] void n00b_crt_main(int, char **, char **);\n"
                "void n00b_crt_run_init_array(void) {}\n"
                "void n00b_init_core_simple(int argc, char **argv) "
                "{ (void)argc; (void)argv; }\n"
                "void n00b_init_late(void) {}\n"
                "void *n00b_crt_apply_comptime_image(void) { return 0; }\n"
                "int n00b_run_degraded_static_inits(void) { return 0; }\n"
                "[[noreturn]] void exit(int rc) { ExitProcess((unsigned int)rc); }\n"
                "[[noreturn]] void n00b_exit(int rc) "
                "{ ExitProcess((unsigned int)rc); }\n"
                "[[noreturn]] void n00b_start(void) { "
                "char *argv[] = {\"test\", 0}; n00b_crt_main(1, argv, 0); }\n",
                encoding="utf-8",
            )

            link_case_obj = work / "link-case.obj"
            failing_case_obj = work / "link-case-fail.obj"
            runtime_obj = work / "runtime.obj"
            runtime_lib = work / "runtime.LIB"
            run_cmd([ncc, *rest, "-c", link_case_src, "-o", link_case_obj])
            run_cmd([ncc, *rest, "-c", failing_case_src, "-o", failing_case_obj])
            run_cmd([ncc, "--no-ncc", "-c", runtime_src, "-o", runtime_obj])
            archiver = shutil.which("llvm-ar") or shutil.which("ar")
            if not archiver:
                fail("llvm-ar or ar is required for Windows link case tests")
            run_cmd([archiver, "rcs", runtime_lib, runtime_obj])

            link_cases = {
                "object-lower": [link_case_obj, runtime_obj],
                "object-upper-metadata": [work / "LINK-CASE.OBJ", runtime_obj],
                "object-upper-replay": [link_case_obj, work / "RUNTIME.OBJ"],
                "library-lower": [link_case_obj, work / "runtime.lib"],
                "library-upper": [link_case_obj, runtime_lib],
            }
            for name, inputs in link_cases.items():
                output = work / f"{name}.exe"
                diagnostic = work / f"{name}.stderr"
                run_cmd(
                    [ncc, *rest, *inputs, "-lkernel32", "-o", output],
                    stderr_path=diagnostic,
                    env_set={"NCC_VERBOSE": "1"},
                )
                check_contains(
                    diagnostic,
                    "--dump-section=.n00bct=",
                    f"{name} skipped NCC metadata processing",
                )
                run_cmd([output])

            for spelling in ("OUT", "out", "OuT"):
                output = work / f"output-{spelling}.exe"
                run_cmd(
                    [
                        ncc,
                        *rest,
                        link_case_obj,
                        runtime_obj,
                        "-lkernel32",
                        f"-Wl,/{spelling}:{output}",
                    ]
                )
                run_cmd([output])

            sentinel = work / "atomic-sentinel.exe"
            sentinel_bytes = b"preserve-existing-output"
            sentinel.write_bytes(sentinel_bytes)
            status = run_cmd(
                [
                    ncc,
                    *rest,
                    failing_case_obj,
                    runtime_obj,
                    "-lkernel32",
                    f"-Wl,/out:{sentinel}",
                ],
                stderr_path=work / "atomic-sentinel.stderr",
                check=False,
            )
            if status == 0:
                fail("failing lowercase /out: comptime link succeeded")
            if sentinel.read_bytes() != sentinel_bytes:
                fail("failing lowercase /out: link modified existing output")

        elif mode == "system_entry":
            if os.name != "nt":
                skip("system-entry fixture is Windows-specific")

            denied_src = pop_arg(rest, "comptime_main source is required")
            allowed_bin = work / "system-entry-allow.exe"
            denied_bin = work / "system-entry-deny.exe"
            system_flags = [*rest, "--ncc-no-comptime", "--ncc-system-entry"]

            run_cmd([ncc, "--no-ncc", "-std=gnu23", "-c", src, "-o", outobj])
            run_cmd([ncc, *system_flags, outobj, "-o", allowed_bin])
            run_cmd([allowed_bin])

            status = run_cmd(
                [ncc, *system_flags, denied_src, "-o", denied_bin],
                stderr_path=stderr_file,
                check=False,
            )
            if status == 0:
                fail("system-entry comptime_main link unexpectedly succeeded")
            check_contains(
                stderr_file,
                "--ncc-system-entry cannot degrade comptime_main; "
                "a custom entry is required",
                "system-entry comptime_main link omitted its diagnostic",
            )
            if denied_bin.exists():
                fail("system-entry comptime_main link produced requested output")

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
