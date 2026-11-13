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
           `python -m vehicle_sil_control --help`, tests/control_server/multi_process.py --help and
           the examples of examples/launch_control with --help

Then, unless --skip-lowest, one more run on 3.11 ("3.11 lowest", build/python-envs/3.11-lowest) with
the lower bounds of pyproject.toml instead of uv.lock: its sync step makes the environment with uv's
lowest-direct resolution of the dev group and fails unless every declared "name>=version" was
installed at exactly that version; then codegen and control as above. So a declared bound is
always one that the tests ran with.

The full output of every step goes to build/logs/<tag>-py<version>[-lowest]-<step>.log. The summary
names the failed step, the first lines of its output and the log file.

Exit codes: 0 every run passed, 1 a step failed, 2 usage error (also when uv is not installed).
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
import tomllib
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

    def note(self, text: str) -> None:
        self.file.write(text + "\n")
        self.file.flush()

    def close(self) -> None:
        self.file.close()


def normalize(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def lower_bounds() -> dict[str, str]:
    """{package: version} of every 'name>=version' requirement in pyproject.toml's dependency groups."""
    with open(ROOT / "pyproject.toml", "rb") as f:
        groups = tomllib.load(f).get("dependency-groups", {})
    bounds = {}
    for items in groups.values():
        for item in items:
            m = re.fullmatch(r"\s*([A-Za-z0-9][A-Za-z0-9._-]*)\s*>=\s*([^\s,;]+)\s*", item) if isinstance(item, str) else None
            if m:
                bounds[normalize(m.group(1))] = m.group(2)
    return bounds


def installed_versions(python: str, env: dict[str, str]) -> dict[str, str]:
    code = ("import importlib.metadata as m\n"
            "for d in m.distributions():\n"
            "    print(d.metadata['Name'], d.version)\n")
    proc = subprocess.run([python, "-c", code], env=env, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return {normalize(n): v for n, v in (ln.split() for ln in proc.stdout.splitlines() if len(ln.split()) == 2)}


def check_version(uv: str, version: str, tag: str, files: list[str],
                  lowest: bool = False) -> tuple[bool, str, list[str], Path | None]:
    """Runs every step for one Python version. Returns (ok, description, failure lines, failed log).

    With lowest, the environment gets the lower bounds of pyproject.toml (uv's lowest-direct resolution,
    not uv.lock), and the sync step fails unless each declared lower bound is what was installed.
    """
    name = f"{version}-lowest" if lowest else version
    env_dir = ENV_DIR / name
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
    resolved: list[str] = []

    def sync(s: Step) -> bool:
        if not lowest:
            return s.run([uv, "sync", "--locked", "--python", version])
        if not (s.run([uv, "venv", "--clear", "--python", version, str(env_dir)])
                and s.run([uv, "pip", "install", "--python", python, "--resolution", "lowest-direct",
                           "--group", "dev"])):
            return False
        bounds = lower_bounds()
        got = installed_versions(python, env)
        ok = True
        for package, bound in sorted(bounds.items()):
            have = got.get(package)
            resolved.append(f"{package} {have}")
            if have != bound:
                s.note(f"error: pyproject.toml declares {package}>={bound}, but the lowest resolution on Python "
                       f"{version} installed {have} (the bound has no usable release there, or another package "
                       f"needs more): the tests never run with {bound}; raise the bound to {have}")
                ok = False
        s.note("lower bounds installed: " + ", ".join(resolved))
        return ok

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
            s.run([python, "-c", "import test_python_client, test_launcher"], cwd=ROOT / "tests" / "control_server"),
            s.run([python, "-m", "vehicle_sil_control", "--help"]),
            s.run([python, "tests/control_server/multi_process.py", "--help"]),
            s.run([python, "examples/launch_control/write_before_start.py", "--help"]),
            s.run([python, "examples/launch_control/pause_and_step.py", "--help"]),
        ])

    steps = [("sync", sync), ("syntax", syntax), ("codegen", codegen), ("scripts", scripts), ("control", control)]
    if lowest:  # the syntax and the scripts do not depend on the packages
        steps = [s for s in steps if s[0] not in ("syntax", "scripts")]
    times = []
    for step_name, body in steps:
        log = LOG_DIR / f"{tag}-py{name}-{step_name}.log"
        step = Step(log, env)
        start = time.monotonic()
        try:
            ok = body(step)
        finally:
            step.close()
        times.append(f"{step_name} {time.monotonic() - start:.0f} s")
        if not ok:
            return False, f"FAILED ({step_name})", first_lines(log), log
    text = f"ok ({', '.join(times)})"
    if resolved:
        text += "\n  with " + ", ".join(resolved)
    return True, text, [], None


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
        "  scripts/verify-python.py                  # 3.11, .python-version, and 3.11 with the lower bounds\n"
        "  scripts/verify-python.py --skip-lowest    # only the versions of uv.lock\n"
        "  scripts/verify-python.py --python 3.13\n",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--python", action="append", metavar="VERSION",
                        help=f"a Python version to check (repeatable; default: {LOWEST} and .python-version"
                        + (f", {pinned}" if pinned else "") + ")")
    parser.add_argument("--skip-lowest", action="store_true",
                        help=f"leave out the run on Python {LOWEST} with the lower bounds of pyproject.toml "
                        "(uv's lowest-direct resolution; it needs the package index or uv's cache)")
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
    runs = [(v, False) for v in versions] + ([] if args.skip_lowest else [(LOWEST, True)])
    failed = []
    try:
        for v, lowest in runs:
            label = f"{v} lowest" if lowest else v
            ok, text, lines, log = check_version(uv, v, args.tag, files, lowest)
            print(f"python {label}: {text}")
            if not ok:
                failed.append(label)
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
    print(f"verify-python: {len(runs)} of {len(runs)} runs passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
