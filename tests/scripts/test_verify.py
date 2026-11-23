# SPDX-License-Identifier: MIT
"""Tests of scripts/verify.py --summarize on made-up ctest logs: a failing test whose output is longer than the
lines the summary shows still has its FAIL line in the summary, below its first lines; a short output is shown
whole, once.

    python tests/scripts/test_verify.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import job_summary  # noqa: E402
import verify  # noqa: E402

VERIFY = ROOT / "scripts" / "verify.py"
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
        print(f"FAILED: {what}")


def ctest_log(output: list[str]) -> str:
    """A ctest log (--output-on-failure) in which launch_run.long fails with `output` and launch_run.fine passes."""
    return "\n".join([
        "Test project /work/build/linux-gcc-debug",
        "    Start 1: launch_run.long",
        "    Start 2: launch_run.fine",
        "1/2 Test #2: launch_run.fine .................   Passed    0.50 sec",
        "2/2 Test #1: launch_run.long .................***Failed   12.00 sec",
        *output,
        "",
        "50% tests passed, 1 tests failed out of 2",
        "",
        "The following tests FAILED:",
        "\t  1 - launch_run.long (Failed)",
        "Errors while running CTest",
        "",
    ])


def summarize(text: str) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as d:
        log = Path(d) / "verify-linux-gcc-debug-test.log"
        log.write_text(text, encoding="utf-8")
        env = {k: v for k, v in os.environ.items()
               if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
        return subprocess.run([sys.executable, str(VERIFY), "--summarize", str(log)], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL, env=env)


def test_a_fail_line_far_down_is_shown() -> None:
    output = [f"[launch_run] step {i}" for i in range(40)]
    output[30] = "FAIL: the JSON has no traffic before the findings"
    output[35] = "Error: Engine left the simulation"
    run = summarize(ctest_log(output))
    out = run.stdout
    check(run.returncode == 1, f"a failed test: exit code 1, not {run.returncode}")
    check("    [launch_run] step 0\n" in out and "    [launch_run] step 14\n" in out,
          "the first 15 lines of the output are shown:\n" + out)
    check("    [launch_run] step 15\n" not in out, "the 16th line is not shown:\n" + out)
    check("    ...\n    FAIL: the JSON has no traffic before the findings\n    Error: Engine left the simulation\n" in out,
          "the FAIL and Error lines further down follow the first lines, after '...':\n" + out)


def test_a_short_output_is_shown_once() -> None:
    output = ["FAIL: Hand ended with 3, as its behaviour says", "[launch_run] the logs:"]
    out = summarize(ctest_log(output)).stdout
    check(out.count("FAIL: Hand ended with 3") == 1 and "    ...\n" not in out,
          "a FAIL line among the first lines is shown once, with no '...':\n" + out)


def test_at_most_five_lines_further_down() -> None:
    output = [f"[launch_run] step {i}" for i in range(15)] + [f"FAIL: check {i}" for i in range(8)]
    out = summarize(ctest_log(output)).stdout
    check("FAIL: check 4\n" in out and "FAIL: check 5" not in out, "five lines further down at most:\n" + out)


def test_cmake_arg_is_forwarded_to_configure() -> None:
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        bin_dir = tmp / "bin"
        bin_dir.mkdir()
        cmake_calls = tmp / "cmake-calls.txt"
        if os.name == "nt":
            (bin_dir / "cmake.bat").write_text(
                f'@echo off\r\necho %* >> "{cmake_calls}"\r\nexit /b 0\r\n', encoding="utf-8")
            (bin_dir / "ctest.bat").write_text(
                '@echo off\r\necho 100%% tests passed out of 1\r\nexit /b 0\r\n', encoding="utf-8")
        else:
            cmake_sh = bin_dir / "cmake"
            cmake_sh.write_text(f"#!/bin/sh\nprintf '%s\\n' \"$*\" >> {cmake_calls!s}\nexit 0\n", encoding="utf-8")
            cmake_sh.chmod(0o755)
            ctest_sh = bin_dir / "ctest"
            ctest_sh.write_text("#!/bin/sh\necho '100% tests passed out of 1'\nexit 0\n", encoding="utf-8")
            ctest_sh.chmod(0o755)
        env = {k: v for k, v in os.environ.items()
               if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
        env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        preset = "windows-mingw-debug" if os.name == "nt" else "linux-gcc-debug"
        run = subprocess.run(
            [sys.executable, str(VERIFY), preset, "--tag", "test-cmake-arg",
             "--cmake-arg", "-DCMAKE_C_FLAGS_DEBUG=-g1", "--cmake-arg=-DCMAKE_CXX_FLAGS_DEBUG=-g1"],
            capture_output=True, text=True, encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL, env=env,
        )
        check(run.returncode == 0, f"verify.py --cmake-arg exited {run.returncode}: {run.stdout}\n{run.stderr}")
        calls = cmake_calls.read_text(encoding="utf-8").splitlines() if cmake_calls.exists() else []
        check(len(calls) >= 1 and "-DCMAKE_C_FLAGS_DEBUG=-g1" in calls[0] and "-DCMAKE_CXX_FLAGS_DEBUG=-g1" in calls[0],
              f"configure received --cmake-arg values: {calls}")


def test_linux_presets_derived_from_cmake_presets() -> None:
    known = verify.load_presets()
    check("windows-mingw-control" in known and known["windows-mingw-control"]["host"] == "Windows",
          f"windows-mingw-control present with Windows host: {known.get('windows-mingw-control')}")
    check(verify.default_linux_presets(known) == [
        "linux-gcc-debug",
        "linux-gcc-release",
        "linux-clang-debug",
        "linux-clang-asan",
        "linux-clang-tsan",
    ], f"default_linux_presets mismatch: {verify.default_linux_presets(known)}")
    all_linux = verify.all_linux_presets(known)
    for expected in (
        "linux-gcc-debug",
        "linux-gcc-release",
        "linux-gcc-static",
        "linux-clang-debug",
        "linux-gcc-asan",
        "linux-gcc-tsan",
        "linux-clang-asan",
        "linux-clang-tsan",
        "linux-gcc-control",
    ):
        check(expected in all_linux, f"{expected} missing from all_linux_presets: {all_linux}")
    check(len(all_linux) == 9, f"expected 9 Linux presets, got {len(all_linux)}: {all_linux}")
    check(known["linux-gcc-tsan"]["test_jobs"] == 8 and known["linux-clang-tsan"]["test_jobs"] == 8,
          f"tsan presets inherit test_jobs=8: {known['linux-gcc-tsan']}, {known['linux-clang-tsan']}")


def test_user_presets_ignored_by_linux_flags() -> None:
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        (tmp / "CMakePresets.json").write_text(json.dumps({
            "version": 3,
            "configurePresets": [
                {"name": "linux-base", "hidden": True,
                 "condition": {"type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux"}},
                {"name": "proj-default", "inherits": "linux-base",
                 "vendor": {"vehicle_sil": {"linux_default": True}}},
                {"name": "proj-extra", "inherits": "linux-base"},
            ],
            "buildPresets": [
                {"name": "proj-default", "configurePreset": "proj-default"},
                {"name": "proj-extra", "configurePreset": "proj-extra"},
            ],
            "testPresets": [
                {"name": "test-base", "hidden": True, "execution": {"jobs": 8}},
                {"name": "proj-default", "inherits": "test-base", "configurePreset": "proj-default"},
                {"name": "proj-extra", "inherits": "test-base", "configurePreset": "proj-extra"},
            ],
        }), encoding="utf-8")
        (tmp / "CMakeUserPresets.json").write_text(json.dumps({
            "version": 3,
            "configurePresets": [
                {"name": "user-inherits-default", "inherits": "proj-default"},
                {"name": "user-claims-default", "inherits": "linux-base",
                 "vendor": {"vehicle_sil": {"linux_default": True}, "vscode": {"flag": 1}}},
                {"name": "user-configure-only", "inherits": "linux-base"},
            ],
            "buildPresets": [
                {"name": "user-inherits-default", "configurePreset": "user-inherits-default"},
                {"name": "user-claims-default", "configurePreset": "user-claims-default"},
                {"name": "user-configure-only", "configurePreset": "user-configure-only"},
            ],
            "testPresets": [
                {"name": "user-inherits-default", "inherits": "test-base", "configurePreset": "user-inherits-default"},
                {"name": "user-claims-default", "inherits": "test-base", "configurePreset": "user-claims-default"},
            ],
        }), encoding="utf-8")
        known = verify.load_presets(tmp)
        check("user-inherits-default" in known and known["user-inherits-default"]["host"] == "Linux",
              f"user preset inherits host: {known.get('user-inherits-default')}")
        check(known["user-inherits-default"]["test_jobs"] == 8,
              f"user preset inherits test_jobs: {known.get('user-inherits-default')}")
        check("user-configure-only" not in known, f"configure-only preset excluded: {known}")
        check(verify.default_linux_presets(known) == ["proj-default"],
              f"user presets excluded from default_linux_presets: {verify.default_linux_presets(known)}")
        check(verify.all_linux_presets(known) == ["proj-default", "proj-extra"],
              f"user presets excluded from all_linux_presets: {verify.all_linux_presets(known)}")


def test_help_and_all_linux_cli() -> None:
    env = {k: v for k, v in os.environ.items()
           if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
    help_run = subprocess.run([sys.executable, str(VERIFY), "--help"], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL, env=env)
    check(help_run.returncode == 0, f"--help exited {help_run.returncode}: {help_run.stderr}")
    for token in ("--linux", "--all-linux", "linux-gcc-debug", "linux-gcc-control"):
        check(token in help_run.stdout, f"--help missing {token!r}:\n{help_run.stdout}")
    bad_run = subprocess.run([sys.executable, str(VERIFY), "--summarize", "foo.log", "--all-linux"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace",
                             stdin=subprocess.DEVNULL, env=env)
    check(bad_run.returncode == 2, f"--summarize --all-linux exited {bad_run.returncode}, expected 2: {bad_run.stderr}")


if __name__ == "__main__":
    for name, test in list(globals().items()):
        if name.startswith("test_") and callable(test):
            test()
    if failures:
        print(f"test_verify: {len(failures)} failed")
        sys.exit(1)
    print("test_verify: passed")
