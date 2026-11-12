"""Wait while timing-sensitive work runs on this machine (C++ builds and tests, the Windows VM).

The browser and Python tests of the GUI start servers, browsers and timers. Run next to a C++ build (ninja), the
C++ tests (ctest, whose timing tests then fail) or the Windows VM (qemu-system-x86_64), both sides get slow and
flaky. scripts/verify-web.py and scripts/verify-gui.py call wait_until_quiet() before each heavy step.

Processes are found by their own name (/proc/<pid>/comm on Linux, tasklist on Windows, ps elsewhere), never by
a pattern searched in command lines, which would also match the shell that runs this script.
Standard library only (Python 3.11+).
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

# Exact process names, and name prefixes (qemu-system-x86_64; /proc/<pid>/comm keeps 15 characters).
EXACT = ("ctest", "ninja")
PREFIXES = ("qemu-system-x86",)
DEFAULT_LIMIT_S = 20 * 60
POLL_S = 5.0


def _names_linux() -> list[str]:
    names = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                names.append((entry / "comm").read_text(encoding="utf-8", errors="replace").strip())
            except OSError:  # the process ended meanwhile
                pass
    return names


def _names_windows() -> list[str]:
    out = subprocess.run(["tasklist", "/fo", "csv", "/nh"], capture_output=True, text=True, errors="replace").stdout
    names = []
    for line in out.splitlines():
        name = line.split('","', 1)[0].strip('"')
        names.append(name[:-4] if name.lower().endswith(".exe") else name)
    return names


def _names_ps() -> list[str]:
    out = subprocess.run(["ps", "-A", "-o", "comm="], capture_output=True, text=True, errors="replace").stdout
    return [os.path.basename(line.strip()) for line in out.splitlines() if line.strip()]


def busy_processes() -> list[str]:
    """The names of the running processes that make the GUI tests unreliable, sorted, each once."""
    try:
        if Path("/proc/self/comm").exists():
            names = _names_linux()
        elif os.name == "nt":
            names = _names_windows()
        else:
            names = _names_ps()
    except OSError:
        return []
    found = {n for n in names if n in EXACT or n.startswith(PREFIXES)}
    return sorted(found)


def wait_until_quiet(prog: str, limit_s: float = DEFAULT_LIMIT_S, poll_s: float = POLL_S) -> None:
    """Return once none of the processes runs, or after limit_s seconds (with a warning). Prints to stderr.

    One line when the wait starts (what it waits for), and a warning if it gives up.
    """
    busy = busy_processes()
    if not busy:
        return
    start = time.monotonic()
    print(f"{prog}: waiting for {', '.join(busy)} to end (timing-sensitive; up to {limit_s / 60:g} min, "
          "--no-wait to skip)", file=sys.stderr, flush=True)
    while busy:
        if time.monotonic() - start >= limit_s:
            print(f"{prog}: warning: {', '.join(busy)} still running after {limit_s / 60:g} min; continuing, "
                  "so timing-sensitive tests may fail", file=sys.stderr, flush=True)
            return
        time.sleep(poll_s)
        busy = busy_processes()


if __name__ == "__main__":
    print("\n".join(busy_processes()) or "(none)")
