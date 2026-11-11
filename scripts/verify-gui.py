#!/usr/bin/env python3
"""Check the browser GUI as a whole: the server's tests, then the screen (scripts/verify-web.py), with a short summary.

Steps, in order:

  sync    uv sync --locked (fails when uv.lock does not match the pyproject.toml files)
  server  uv run pytest gui/server/tests: the GUI server against the fake launcher (API, WebSocket, security,
          stopping, and the comparison of gui/protocol/examples); JUnit XML in build/logs/<tag>-server.xml
  web     scripts/verify-web.py --json: npm ci, format, lint, unit tests, build and e2e of gui/web; its steps
          are listed under this one, with their logs

The full output of each step goes to build/logs/<tag>-<step>.log (the web steps: build/logs/<tag>-web-*.log).
The summary shows one line per step and, for a failed step, its first error lines and its log. Stops at the
first failed step. A stopped run (Ctrl-C, SIGTERM) stops the whole process group of the running step.

Exit codes: 0 every step passed, 1 a step failed, 2 usage error (uv missing; verify-web.py's own usage errors,
such as an unsupported Node, are passed on). Requires Python 3.11+ (standard library only) and uv; the web step
also needs Node and npm (gui/web/README.md).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "build" / "logs"
LINE_CHARS = 300
MAX_ERROR_LINES = 12
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

ERROR_LINES = {
    "sync": re.compile(r"error|lockfile|needs to be updated", re.IGNORECASE),
    "server": re.compile(r"^(FAILED|ERROR) |^E\s+|Error:|^\S+\.py:\d+: "),
    "web": re.compile(r"."),
}
SUMMARY_LINES = {
    "sync": re.compile(r"^(Resolved|Audited|Installed|Checked) "),
    "server": re.compile(r"^=*\s*\d+ (passed|failed)|^\d+ (passed|failed)"),
    "web": re.compile(r"$^"),
}


@dataclass
class Step:
    name: str
    command: list[str]
    ok: bool = True
    code: int = 0
    seconds: float = 0.0
    log: str = ""
    summary: str = ""
    errors: list[str] = field(default_factory=list)
    substeps: list[dict] = field(default_factory=list)

    def to_json(self) -> dict:
        out = {"step": self.name, "ok": self.ok, "exit_code": self.code, "seconds": round(self.seconds, 1),
               "log": self.log, "summary": self.summary, "errors": self.errors}
        if self.substeps:
            out["steps"] = self.substeps
        return out


def die_usage(message: str) -> None:
    print(f"verify-gui: error: {message}", file=sys.stderr)
    sys.exit(2)


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def clean_lines(text: str) -> list[str]:
    return [ANSI.sub("", ln).rstrip()[:LINE_CHARS] for ln in text.splitlines()]


def stop_group(process: subprocess.Popen) -> None:
    """Stop the step and everything it started (the GUI servers of the tests, npm, browsers)."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True)
        return
    for sig, seconds in ((signal.SIGINT, 20), (signal.SIGTERM, 5), (signal.SIGKILL, 0)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                return
            time.sleep(0.2)


def run_step(step: Step, env: dict[str, str], log: Path) -> None:
    step.log = rel(log)
    start = time.monotonic()
    group = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write("$ " + " ".join(step.command) + "\n")
        f.flush()
        # The web step's JSON summary is read from stdout; its own logs have the details.
        stdout = subprocess.PIPE if step.name == "web" else f
        process = subprocess.Popen(step.command, cwd=ROOT, env=env, stdout=stdout, stderr=f,
                                   stdin=subprocess.DEVNULL, **group)
        try:
            out = process.communicate()[0] if step.name == "web" else None
            step.code = process.wait()
        except BaseException:
            stop_group(process)
            raise
        if out is not None:
            f.write(out.decode("utf-8", "replace"))
    step.seconds = time.monotonic() - start
    step.ok = step.code == 0
    lines = clean_lines(log.read_text(encoding="utf-8", errors="replace"))
    if step.name == "web":
        read_web(step, out.decode("utf-8", "replace") if out else "", lines)
        return
    found = [ln.strip(" =") for ln in lines if SUMMARY_LINES[step.name].search(ln)]
    step.summary = found[-1] if found else ""
    if not step.ok:
        errors = [ln for ln in lines[1:] if ln.strip() and ERROR_LINES[step.name].search(ln)]
        step.errors = errors[:MAX_ERROR_LINES] or [ln for ln in lines[1:] if ln.strip()][-8:]
        if step.name == "server" and step.code == 5:
            step.errors.append("no test was run: check the -k expression")


def read_web(step: Step, out: str, lines: list[str]) -> None:
    try:
        summary = json.loads(out)
        step.substeps = summary["steps"]
    except (ValueError, KeyError, TypeError):
        step.errors = [ln for ln in lines[1:] if ln.strip()][-8:]  # a usage error: verify-web said why
        return
    passed = sum(s["ok"] for s in step.substeps)
    step.summary = f"{passed} of {len(step.substeps)} steps passed"
    failed = [s for s in step.substeps if not s["ok"]]
    if failed:
        step.errors = failed[0]["errors"]


def on_sigterm(_signum, _frame) -> None:
    raise KeyboardInterrupt


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/verify-gui.py",
        description="Run the GUI server's tests and then scripts/verify-web.py, and print a short summary; the full "
        "output goes to build/logs/<tag>-*.log. Exit code 0 if every step passed, 1 if one failed, 2 on a usage error.",
        epilog="examples:\n"
        "  scripts/verify-gui.py                    # everything\n"
        "  scripts/verify-gui.py --skip-web         # only the server (no Node needed)\n"
        "  scripts/verify-gui.py -k security        # only the server tests whose names match\n"
        "  scripts/verify-gui.py --skip-install --skip-e2e   # passed on to verify-web.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--skip-web", action="store_true", help="do not run scripts/verify-web.py")
    parser.add_argument("--skip-install", action="store_true", help="passed on to verify-web.py (keep node_modules)")
    parser.add_argument("--skip-e2e", action="store_true", help="passed on to verify-web.py (no browser tests)")
    parser.add_argument("-k", dest="keyword", help="only the server tests matching this pytest -k expression")
    parser.add_argument("--tag", default="gui", help="prefix of the log names (default: gui)")
    parser.add_argument("--json", action="store_true",
                        help='print the summary as JSON ({"vehicle_sil_verify_gui": 1, "ok": ..., "steps": [...]})')
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.tag):
        die_usage(f"--tag {args.tag!r}: use letters, digits, '.', '_' and '-' only (it becomes part of file names)")
    uv = shutil.which("uv")
    if uv is None:
        die_usage("uv is not on PATH: install it (https://docs.astral.sh/uv/), then run this again")

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    junit = LOG_DIR / f"{args.tag}-server.xml"
    pytest = [uv, "run", "--locked", "pytest", "gui/server/tests", "-q",
              f"--junitxml={junit}"]
    if args.keyword:
        pytest += ["-k", args.keyword]
    steps = [Step("sync", [uv, "sync", "--locked"]), Step("server", pytest)]
    if not args.skip_web:
        web = [sys.executable, str(ROOT / "scripts" / "verify-web.py"), "--json", "--tag", f"{args.tag}-web"]
        web += ["--skip-install"] * args.skip_install + ["--skip-e2e"] * args.skip_e2e
        steps.append(Step("web", web))

    env = {**os.environ, "NO_COLOR": "1", "PYTHONDONTWRITEBYTECODE": "1"}
    env.pop("VIRTUAL_ENV", None)
    logs = {s.name: LOG_DIR / f"{args.tag}-{s.name}.log" for s in steps}
    for stale in [*logs.values(), junit]:
        stale.unlink(missing_ok=True)
    signal.signal(signal.SIGTERM, on_sigterm)

    done: list[Step] = []
    try:
        for step in steps:
            run_step(step, env, logs[step.name])
            done.append(step)
            if not args.json:
                head = f"gui {step.name}: " + ("ok" if step.ok else f"FAILED (exit {step.code})")
                head += f" ({step.seconds:.0f} s)"
                if step.summary:
                    head += f" - {step.summary}"
                print(head)
                for sub in step.substeps:
                    line = f"  web {sub['step']}: " + ("ok" if sub["ok"] else f"FAILED (exit {sub['exit_code']})")
                    line += f" ({sub['seconds']:.0f} s)" + (f" - {sub['summary']}" if sub["summary"] else "")
                    print(line)
                    if not sub["ok"]:
                        print(f"    log: {sub['log']}")
                for ln in step.errors:
                    print("    " + ln)
                if not step.ok:
                    print(f"  log: {step.log}")
                sys.stdout.flush()
            if not step.ok:
                break
    except KeyboardInterrupt:
        print("verify-gui: interrupted", file=sys.stderr)
        return 130

    failed = [s for s in done if not s.ok]
    if args.json:
        print(json.dumps({"vehicle_sil_verify_gui": 1, "ok": not failed, "steps": [s.to_json() for s in done]},
                         indent=2, ensure_ascii=False))
    elif failed:
        print(f"verify-gui: FAILED {failed[0].name}")
    else:
        print(f"verify-gui: {len(done)} of {len(done)} steps passed")
    if failed and failed[0].name == "web" and failed[0].code == 2:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
