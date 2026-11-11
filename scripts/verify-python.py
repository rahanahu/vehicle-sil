#!/usr/bin/env python3
"""Check the Python code with uv on the lowest supported Python and on the pinned one, and print a short summary.

For each Python version (default: 3.11, the lowest that pyproject.toml allows, and the one in
.python-version) it makes an environment from uv.lock in build/python-envs/<version> and runs what
needs no C++ build:

  sync     uv sync --locked (fails when uv.lock does not match pyproject.toml)
  syntax   compiles every tracked *.py file
  codegen  tools/j1939_codegen/test_j1939_codegen.py
  scripts  scripts/*.py --help
  control  generates the gRPC stubs of the Control API, imports the client and its tests, and runs
           `python -m vehicle_sil_control --help` and tests/control_server/multi_process.py --help

The full output of every step goes to build/logs/<tag>-py<version>-<step>.log. The summary names the
failed step, the first lines of its output and the log file.

Exit codes: 0 every version passed, 1 a step failed, 2 usage error (also when uv is not installed).
Requires Python 3.11+ (standard library only) and uv; runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "build" / "logs"
ENV_DIR = ROOT / "build" / "python-envs"
LOWEST = "3.11"
OUTPUT_LINES = 15
LINE_CHARS = 400


def die_usage(message: str) -> None:
    print(f"verify-python: error: {message}", file=sys.stderr)
    sys.exit(2)


def pinned_version() -> str | None:
    path = ROOT / ".python-version"
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            return line
    return None


def tracked_python_files() -> list[str]:
    proc = subprocess.run(["git", "-c", "core.quotePath=false", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
                           "--", "*.py"], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode == 0:
        return sorted(p for p in proc.stdout.split("\0") if p and (ROOT / p).is_file())
    # Not a git checkout (a source archive): the directories that hold Python code.
    dirs = ["python", "scripts", "tests", "tools"]
    return sorted(p.relative_to(ROOT).as_posix() for d in dirs for p in (ROOT / d).rglob("*.py"))


def env_python(env: Path) -> Path:
    if os.name == "nt":
        return env / "Scripts" / "python.exe"
    return env / "bin" / "python"


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def first_lines(log: Path) -> list[str]:
    """The end of a failed step's output, where the error is (the start is the command lines)."""
    lines = [ln for ln in log.read_text(encoding="utf-8", errors="replace").splitlines() if ln.strip()]
    lines = lines[-OUTPUT_LINES:]
    return [ln if len(ln) <= LINE_CHARS else ln[:LINE_CHARS] + " ..." for ln in lines]


class Step:
    def __init__(self, log: Path, env: dict[str, str]):
        self.log = log
        self.env = env
        self.file = open(log, "w", encoding="utf-8", errors="replace")

    def run(self, cmd: list[str], cwd: Path = ROOT) -> bool:
        self.file.write("$ " + " ".join(cmd) + "\n")
        self.file.flush()
        code = subprocess.run(cmd, cwd=cwd, env=self.env, stdout=self.file, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL).returncode
        if code != 0:
            self.file.write(f"(exit code {code})\n")
        self.file.flush()
        return code == 0

    def close(self) -> None:
        self.file.close()


def check_version(uv: str, version: str, tag: str, files: list[str]) -> tuple[bool, str, list[str], Path | None]:
    """Runs every step for one Python version. Returns (ok, description, failure lines, failed log)."""
    env_dir = ENV_DIR / version
    python = str(env_python(env_dir))
    stubs = env_dir / "control-stubs"
    env = dict(os.environ)
    env.update({
        "UV_PROJECT_ENVIRONMENT": str(env_dir),
        "PYTHONDONTWRITEBYTECODE": "1",
        "PYTHONPATH": str(ROOT / "python"),
        "VEHICLE_SIL_CONTROL_STUBS": str(stubs),
    })
    env.pop("VIRTUAL_ENV", None)  # uv would warn that it does not match UV_PROJECT_ENVIRONMENT

    def sync(s: Step) -> bool:
        return s.run([uv, "sync", "--locked", "--python", version])

    def syntax(s: Step) -> bool:
        code = ("import sys\n"
                "for f in sys.argv[1:]:\n"
                "    compile(open(f, 'rb').read(), f, 'exec', dont_inherit=True)\n"
                "print(len(sys.argv) - 1, 'files compiled')\n")
        return s.run([python, "-c", code, *files])

    def codegen(s: Step) -> bool:
        return s.run([python, "tools/j1939_codegen/test_j1939_codegen.py"])

    def scripts(s: Step) -> bool:
        return all([s.run([python, p, "--help"]) for p in files if p.startswith("scripts/")])

    def control(s: Step) -> bool:
        if not s.run([python, "-m", "vehicle_sil_control.generate", "--out", str(stubs)]):
            return False
        return all([
            s.run([python, "-c", "import vehicle_sil_control as c; c.Control; print('imported', c.__name__)"]),
            s.run([python, "-c", "import test_python_client"], cwd=ROOT / "tests" / "control_server"),
            s.run([python, "-m", "vehicle_sil_control", "--help"]),
            s.run([python, "tests/control_server/multi_process.py", "--help"]),
        ])

    steps = [("sync", sync), ("syntax", syntax), ("codegen", codegen), ("scripts", scripts), ("control", control)]
    times = []
    for name, body in steps:
        log = LOG_DIR / f"{tag}-py{version}-{name}.log"
        step = Step(log, env)
        start = time.monotonic()
        try:
            ok = body(step)
        finally:
            step.close()
        times.append(f"{name} {time.monotonic() - start:.0f} s")
        if not ok:
            return False, f"FAILED ({name})", first_lines(log), log
    return True, f"ok ({', '.join(times)})", [], None


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    pinned = pinned_version()
    parser = argparse.ArgumentParser(
        prog="scripts/verify-python.py",
        description="Check the Python code (J1939 code generator, Control API client, scripts) with uv on "
        "several Python versions and print a short summary; the full output goes to build/logs/. "
        "Exit code 0 if every version passed, 1 if a step failed, 2 on a usage error or without uv.",
        epilog="examples:\n"
        "  scripts/verify-python.py                  # 3.11 and the version in .python-version\n"
        "  scripts/verify-python.py --python 3.13\n",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--python", action="append", metavar="VERSION",
                        help=f"a Python version to check (repeatable; default: {LOWEST} and .python-version"
                        + (f", {pinned}" if pinned else "") + ")")
    parser.add_argument("--tag", default="verify", help="prefix of the log names (default: verify)")
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.tag):
        die_usage(f"--tag {args.tag!r}: use letters, digits, '.', '_' and '-' only (it becomes part of file names)")
    versions = list(dict.fromkeys(args.python or [LOWEST] + ([pinned] if pinned else [])))
    for v in versions:
        if not re.fullmatch(r"3\.\d+(\.\d+)?", v):
            die_usage(f"--python {v!r}: give a CPython version such as 3.11 or 3.14.2")
    uv = shutil.which("uv")
    if uv is None:
        die_usage("uv is not on PATH; install it (https://docs.astral.sh/uv/getting-started/installation/). "
                  "Without uv, install the packages of pyproject.toml's [dependency-groups] with pip "
                  "or the OS packages and run the tests directly")

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    files = tracked_python_files()
    failed = []
    try:
        for v in versions:
            ok, text, lines, log = check_version(uv, v, args.tag, files)
            print(f"python {v}: {text}")
            if not ok:
                failed.append(v)
                print("  ---")
                for ln in lines:
                    print("    " + ln)
                print(f"  log: {rel(log)}")
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("verify-python: interrupted", file=sys.stderr)
        return 130
    if failed:
        print("verify-python: FAILED " + ", ".join(failed))
        return 1
    print(f"verify-python: {len(versions)} of {len(versions)} Python versions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
