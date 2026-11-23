"""Shared helpers for scripts/verify*.py (paths, stdio, usage errors, tag validation, process groups).

Standard library only (Python 3.11+); runs on Linux and Windows.
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "build" / "logs"
LINE_CHARS = 300
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
TAG_PATTERN = re.compile(r"[A-Za-z0-9._-]+")


def reconfigure_stdio(*, encoding: str | None = None, stderr: bool = False) -> None:
    streams = (sys.stdout, sys.stderr) if stderr else (sys.stdout,)
    kwargs: dict[str, str] = {"errors": "backslashreplace"}
    if encoding is not None:
        kwargs["encoding"] = encoding
    for stream in streams:
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(**kwargs)


def die_usage(prog: str, message: str) -> NoReturn:
    print(f"{prog}: error: {message}", file=sys.stderr)
    sys.exit(2)


def rel(path: Path, root: Path = ROOT) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def validate_tag(prog: str, tag: str) -> str:
    if not TAG_PATTERN.fullmatch(tag):
        die_usage(prog, f"--tag {tag!r}: use letters, digits, '.', '_' and '-' only (it becomes part of file names)")
    return tag


def clean_lines(text: str, limit: int = LINE_CHARS) -> list[str]:
    return [ANSI.sub("", ln).rstrip()[:limit] for ln in text.splitlines()]


def process_group_kwargs() -> dict[str, object]:
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {"start_new_session": True}


def _group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_group(process: subprocess.Popen) -> None:
    """Stop a process and everything in its process group."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True)
        return
    # SIGINT first: Playwright then stops the web servers it started (they run in their own process groups).
    for sig, seconds in ((signal.SIGINT, 20), (signal.SIGTERM, 5), (signal.SIGKILL, 0)):
        if not _group_alive(process.pid):
            return
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and _group_alive(process.pid):
            time.sleep(0.2)


def _on_sigterm(_signum: int, _frame: object) -> None:
    raise KeyboardInterrupt


def install_sigterm_handler() -> None:
    signal.signal(signal.SIGTERM, _on_sigterm)


if __name__ == "__main__":
    if "-h" in sys.argv[1:] or "--help" in sys.argv[1:]:
        print((__doc__ or "").strip())
    sys.exit(0)
