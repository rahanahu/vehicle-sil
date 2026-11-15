# SPDX-License-Identifier: MIT
"""Tests of scripts/verify.py --summarize on made-up ctest logs: a failing test whose output is longer than the
lines the summary shows still has its FAIL line in the summary, below its first lines; a short output is shown
whole, once.

    python tests/scripts/test_verify.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import job_summary  # noqa: E402

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


if __name__ == "__main__":
    for name, test in list(globals().items()):
        if name.startswith("test_") and callable(test):
            test()
    if failures:
        print(f"test_verify: {len(failures)} failed")
        sys.exit(1)
    print("test_verify: passed")
