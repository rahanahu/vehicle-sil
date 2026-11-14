# SPDX-License-Identifier: MIT
"""Tests of scripts/job_summary.py: what a script appends to GitHub Actions' job summary (GITHUB_STEP_SUMMARY),
when it appends nothing, and that a summary file that cannot be written fails nothing. scripts/check-docs.py on
made-up Markdown files stands for every script that CI calls.

    python tests/scripts/test_job_summary.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import job_summary  # noqa: E402

CHECK_DOCS = ROOT / "scripts" / "check-docs.py"
failures: list[str] = []


def check(condition: bool, what: str) -> None:
    if not condition:
        failures.append(what)
        print(f"FAILED: {what}")


def check_docs(summary: Path | str | None, *args: str, **extra: str) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items()
           if k not in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
    if summary is not None:
        env[job_summary.SUMMARY_ENV] = str(summary)
    env.update(extra)
    return subprocess.run([sys.executable, str(CHECK_DOCS), *args], env=env, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL)


def docs(root: Path) -> tuple[Path, Path]:
    (root / "target.md").write_text("# Target\n", encoding="utf-8")
    good = root / "good.md"
    good.write_text("See [the target](target.md).\n", encoding="utf-8")
    bad = root / "bad.md"
    bad.write_text("See [nothing](missing.md).\n", encoding="utf-8")
    return good, bad


def test_passed_and_failed(root: Path) -> None:
    good, bad = docs(root)
    summary = root / "summary.md"
    p = check_docs(summary, str(good))
    check(p.returncode == 0, f"check-docs on a good file passes (exit {p.returncode}: {p.stdout}{p.stderr})")
    text = summary.read_text(encoding="utf-8")
    check(text.startswith(f"### `check-docs.py {good}`: passed\n"), f"a heading per run, with the command: {text!r}")
    check(f"```text\n{p.stdout.strip()}\n```" in text, f"the printed summary as a code block: {text!r}")
    check("artifact" not in text and "logs" not in text, f"no note on the logs when it passed: {text!r}")

    p = check_docs(summary, str(bad), **{job_summary.ARTIFACT_ENV: "logs-python"})
    check(p.returncode == 1, f"check-docs on a broken link fails (exit {p.returncode})")
    after = summary.read_text(encoding="utf-8")
    check(after.startswith(text), "appended, the run before is kept")
    added = after[len(text):]
    check(added.startswith(f"### `check-docs.py {bad}`: FAILED (exit 1)\n"), f"the failure's heading: {added!r}")
    check("missing.md" in added and p.stdout.strip() in added, f"the failure's lines as printed: {added!r}")
    check("artifact `logs-python`" in added, f"the failure names the artifact of the logs: {added!r}")
    check("###" not in p.stdout and p.stdout.startswith(str(bad)), f"the terminal output is unchanged: {p.stdout!r}")


def test_usage_error_is_written(root: Path) -> None:
    summary = root / "summary.md"
    p = check_docs(summary, str(root / "absent.md"))
    check(p.returncode == 2, f"a file that does not exist is a usage error (exit {p.returncode})")
    text = summary.read_text(encoding="utf-8") if summary.exists() else ""
    check("FAILED (exit 2)" in text and "no such file" in text, f"the usage error, with its message: {text!r}")
    check("The full logs are the files under build/logs" in text, f"without an artifact, says where logs are: {text!r}")


def test_nothing_written(root: Path) -> None:
    good, _ = docs(root)
    summary = root / "summary.md"
    p = check_docs(None, str(good))
    check(p.returncode == 0 and not summary.exists(), "without GITHUB_STEP_SUMMARY nothing is written")
    p = check_docs("", str(good))
    check(p.returncode == 0 and not any(root.glob("*summary*")), "an empty GITHUB_STEP_SUMMARY writes nothing")
    for args in (["--help"], ["-h"]):
        p = check_docs(summary, *args)
        check(p.returncode == 0 and not summary.exists(), f"{args[0]} writes nothing (exit {p.returncode})")
    p = check_docs(summary, str(good), **{job_summary.NESTED_ENV: "1"})
    check(p.returncode == 0 and not summary.exists(), "a script run by another script leaves the summary to it")


def test_unwritable_summary(root: Path) -> None:
    good, bad = docs(root)
    for where in (root, root / "no" / "such" / "dir" / "summary.md"):  # a directory, a path with no parent
        p = check_docs(where, str(good))
        check(p.returncode == 0 and "check-docs: ok" in p.stdout and not p.stderr,
              f"a summary that cannot be written ({where.name}) fails nothing (exit {p.returncode}: {p.stderr})")
        p = check_docs(where, str(bad))
        check(p.returncode == 1, f"and keeps a failure a failure (exit {p.returncode})")


def test_in_process(root: Path) -> None:
    summary = root / "summary.md"
    saved = {k: os.environ.get(k) for k in (job_summary.SUMMARY_ENV, job_summary.NESTED_ENV, job_summary.ARTIFACT_ENV)}
    os.environ[job_summary.SUMMARY_ENV] = str(summary)
    os.environ.pop(job_summary.NESTED_ENV, None)
    os.environ.pop(job_summary.ARTIFACT_ENV, None)
    try:
        def main() -> int:
            sys.stdout.reconfigure(errors="backslashreplace")  # verify.py does this: passed on to the stream
            print("a line with ``` in it")
            print("a warning", file=sys.stderr)
            return 3

        terminal = io.StringIO()
        with contextlib.redirect_stdout(terminal), contextlib.redirect_stderr(terminal):
            terminal.reconfigure = lambda **_: None
            code = job_summary.run(main, ["scripts/x.py", "--tag", "ci"])
        check(code == 3, f"the exit code is main's ({code})")
        check(terminal.getvalue() == "a line with ``` in it\na warning\n", f"the terminal gets it all: {terminal.getvalue()!r}")
        text = summary.read_text(encoding="utf-8")
        check(text.startswith("### `x.py --tag ci`: FAILED (exit 3)\n"), f"heading: {text!r}")
        check("````text\na line with ``` in it\na warning\n````" in text, f"a fence longer than any in the text: {text!r}")
        check(os.environ.get(job_summary.NESTED_ENV) == "1", "the scripts it runs are told not to write")

        os.environ.pop(job_summary.NESTED_ENV, None)
        summary.unlink()

        def exits() -> int:
            sys.exit(2)

        try:
            with contextlib.redirect_stdout(io.StringIO()):
                job_summary.run(exits, ["scripts/x.py"])
            check(False, "sys.exit inside main is passed on")
        except SystemExit as e:
            check(e.code == 2, f"with its code ({e.code})")
        check("### `x.py`: FAILED (exit 2)" in summary.read_text(encoding="utf-8"), "and written")

        long = "x" * 80 + "\n"
        cut = job_summary.markdown("y.py", 1, long * (job_summary.MAX_CHARS // len(long) + 50), None)
        check(len(cut) < job_summary.MAX_CHARS + 1000 and "(cut after" in cut, "a long output is cut, its head kept")
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        for test in (test_passed_and_failed, test_usage_error_is_written, test_nothing_written, test_unwritable_summary,
                     test_in_process):
            d = Path(tmp).resolve() / test.__name__
            d.mkdir()
            test(d)
    print(f"{'FAILED' if failures else 'passed'}: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
