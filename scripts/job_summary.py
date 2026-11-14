"""Write a script's summary to the job summary of GitHub Actions, too.

On GitHub Actions the environment variable GITHUB_STEP_SUMMARY names a file; the Markdown appended to it is shown
on the run's page, under the job. The scripts that CI calls end with

    sys.exit(job_summary.run(main))

which runs main() and passes on its exit code. While it runs, what the script prints (stdout and stderr: its
summary, which is short; the full output of builds and tests goes to build/logs) is also kept, and afterwards
appended to that file under a heading of its own: the command, "passed" or "FAILED (exit N)", the printed lines
as a code block and, on a failure, where the logs are (the artifact that VEHICLE_SIL_LOG_ARTIFACT names, set by
the workflow). The terminal output does not change.

Nothing is written when GITHUB_STEP_SUMMARY is unset or empty, for --help and --json (machine-readable output),
or when a script that does this runs the script (VEHICLE_SIL_JOB_SUMMARY_NESTED: scripts/verify-gui.py runs
scripts/verify-web.py, whose steps its own summary shows). A file that cannot be written is left alone: the
summary never fails a check.
Standard library only (Python 3.11+).
"""

from __future__ import annotations

import os
import sys
import traceback
from pathlib import Path
from typing import Callable

SUMMARY_ENV = "GITHUB_STEP_SUMMARY"
ARTIFACT_ENV = "VEHICLE_SIL_LOG_ARTIFACT"
NESTED_ENV = "VEHICLE_SIL_JOB_SUMMARY_NESTED"
# GitHub keeps up to 1 MiB of summary per step; the scripts' summaries are far shorter. A traceback or a script
# that printed more is cut here, keeping its first lines.
MAX_CHARS = 64 * 1024


class _Tee:
    """Writes to the stream and keeps a copy; everything else (reconfigure, isatty, buffer) is the stream's."""

    def __init__(self, stream, kept: list[str]) -> None:
        self._stream = stream
        self._kept = kept

    def write(self, text: str) -> int:
        self._kept.append(text)
        return self._stream.write(text)

    def __getattr__(self, name: str):
        return getattr(self._stream, name)


def _fence(text: str) -> str:
    longest = run = 0
    for ch in text:
        run = run + 1 if ch == "`" else 0
        longest = max(longest, run)
    return "`" * max(3, longest + 1)


def markdown(command: str, code: int, text: str, artifact: str | None) -> str:
    """The Markdown of one script's run: a heading, the printed lines, and where the logs are on a failure."""
    status = "passed" if code == 0 else f"FAILED (exit {code})"
    if len(text) > MAX_CHARS:
        text = text[:MAX_CHARS].rsplit("\n", 1)[0] + f"\n... (cut after {MAX_CHARS} characters; the step's log has all)"
    text = text.rstrip("\n")
    shown = command.replace("`", "'")  # a code span, so that the regexes and paths stay as typed
    out = [f"### `{shown}`: {status}", ""]
    if text:
        fence = _fence(text)
        out += [fence + "text", text, fence, ""]
    if code != 0:
        if artifact:
            out += [f"The full logs (the files under build/logs named above) are in the artifact `{artifact}` of "
                    "this run.", ""]
        else:
            out += ["The full logs are the files under build/logs named above.", ""]
    return "\n".join(out) + "\n"


def append(path: str, content: str) -> None:
    """Appends to the summary file; a file that cannot be written is left alone."""
    try:
        with open(path, "a", encoding="utf-8", errors="replace") as f:
            f.write(content)
    except (OSError, ValueError):
        pass


def _exit_code(code) -> int:
    if code is None:
        return 0
    return code if isinstance(code, int) else 1


def run(main: Callable[[], int | None], argv: list[str] | None = None) -> int:
    """Runs main() and returns its exit code; on GitHub Actions, appends what it printed to the job summary."""
    argv = sys.argv if argv is None else argv
    path = os.environ.get(SUMMARY_ENV, "")
    if (not path or os.environ.get(NESTED_ENV)
            or any(a in ("-h", "--help", "--json") for a in argv[1:])):
        return main()
    os.environ[NESTED_ENV] = "1"  # the scripts that this one runs leave the summary to it
    kept: list[str] = []
    out, err = sys.stdout, sys.stderr
    sys.stdout, sys.stderr = _Tee(out, kept), _Tee(err, kept)
    code: int | None = 1
    exit_value = None
    try:
        code = main()
    except SystemExit as e:  # sys.exit() inside main, e.g. a usage error (2)
        exit_value = e
        code = _exit_code(e.code)
        if isinstance(e.code, str):
            kept.append(e.code + "\n")
    except BaseException:
        kept.append(traceback.format_exc())
        raise
    finally:
        sys.stdout, sys.stderr = out, err
        command = " ".join([Path(argv[0]).name, *argv[1:]])
        append(path, markdown(command, _exit_code(code), "".join(kept), os.environ.get(ARTIFACT_ENV) or None))
    if exit_value is not None:
        raise exit_value
    return _exit_code(code)
