#!/usr/bin/env python3
"""Install, check, unit-test, build and end-to-end test the web front end (gui/web), with a short summary.

Steps, in order: npm ci, npm run format:check, npm run lint, npm test, npm run build, npm run e2e (the built
app served by the GUI server of gui/server, which runs the fake launcher), npm run e2e:dev (the dev server with
its proxy to the GUI server). The full output of each step goes to build/logs/<tag>-<step>.log (tag: web); the
summary shows one line per step and, for a failed step, its first error lines and its log. Stops at the first
failed step. A stopped run (Ctrl-C, SIGTERM) stops the whole process group of the running step, so no dev
server or GUI server is left behind.

Before each heavy step (unit tests, build, e2e) it waits while a C++ build or test run (ninja, ctest) or the
Windows VM (qemu-system-x86*) runs on this machine, because the browser tests are timing-sensitive and so are
the C++ tests (scripts/busy_machine.py). It gives up after --wait-limit minutes and goes on with a warning.

Exit codes: 0 every step passed, 1 a step failed, 2 usage error (missing tools, unsupported Node or npm).
Requires Python 3.11+ (standard library only), Node and npm (gui/web/.nvmrc), and uv for the e2e steps.
On Windows, run it with a Windows Python (python.org or MinGW, where shutil.which finds npm.cmd).
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

from busy_machine import DEFAULT_LIMIT_S, wait_until_quiet

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "gui" / "web"
LOG_DIR = ROOT / "build" / "logs"
LINE_CHARS = 300
MAX_ERROR_LINES = 12

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# Lines worth showing from a failed step, by step.
E2E_ERRORS = re.compile(r"✘|^\s+\d+\) |Error:|Timed out|\d+ (failed|flaky)|webServer|WebServer.*(rror|exit)")
ERROR_LINES = {
    "install": re.compile(r"npm (error|ERR!)|EBADENGINE|ERESOLVE"),
    "format": re.compile(r"\[warn\]|\[error\]"),
    "lint": re.compile(r"^\S.*\.(ts|html)$|^\s+\d+:\d+\s+(error|warning)\s|problems?\s*\(|Error"),
    "test": re.compile(r"\bFAIL\b|×|AssertionError|Error:|^\s+(Test Files|Tests)\s"),
    "build": re.compile(r"^\s*(✘|X)\s*\[ERROR\]|\[ERROR\]|error TS\d+|^Error|budget"),
    "e2e": E2E_ERRORS,
    "e2e-dev": E2E_ERRORS,
}

# A line that sums up a passed step.
E2E_SUMMARY = re.compile(r"^\s+\d+ passed")
SUMMARY_LINES = {
    "install": re.compile(r"^added \d+ packages"),
    "format": re.compile(r"All matched files use Prettier code style"),
    "lint": re.compile(r"All files pass linting"),
    "test": re.compile(r"^\s+Tests\s+\d+"),
    "build": re.compile(r"Initial total"),
    "e2e": E2E_SUMMARY,
    "e2e-dev": E2E_SUMMARY,
}

# Steps that wait while timing-sensitive work runs on the machine.
HEAVY = {"test", "build", "e2e", "e2e-dev"}

# Warnings worth showing even when a step passes.
NOTE_LINES = {"build": re.compile(r"budget", re.IGNORECASE)}


@dataclass
class Step:
    name: str
    command: list[str]
    ok: bool = True
    code: int = 0
    seconds: float = 0.0
    log: str = ""
    summary: str = ""
    notes: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {"step": self.name, "ok": self.ok, "exit_code": self.code, "seconds": round(self.seconds, 1),
                "log": self.log, "summary": self.summary, "notes": self.notes, "errors": self.errors}


def die_usage(message: str) -> None:
    print(f"verify-web: error: {message}", file=sys.stderr)
    sys.exit(2)


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def clean_lines(text: str) -> list[str]:
    return [ANSI.sub("", ln).rstrip()[:LINE_CHARS] for ln in text.splitlines()]


# --- versions -----------------------------------------------------------------------------------


def parse_version(text: str) -> tuple[int, int, int] | None:
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", text)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None


def satisfies(version: tuple[int, int, int], spec: str) -> bool | None:
    """Check a version against an engines range made of '^X.Y.Z', '>=X.Y.Z' and '||'. None: not understood."""
    for alternative in spec.split("||"):
        alternative = alternative.strip()
        m = re.fullmatch(r"(\^|>=)\s*(\d+)\.(\d+)\.(\d+)", alternative)
        if not m:
            return None
        low = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
        if version >= low and (m.group(1) == ">=" or version[0] == low[0]):
            return True
    return False


def check_engine(tool: str, path: str, spec: str) -> None:
    out = subprocess.run([path, "--version"], capture_output=True, text=True).stdout.strip()
    version = parse_version(out)
    if version is None:
        die_usage(f"cannot read the version of {tool} from {out!r}")
    ok = satisfies(version, spec)
    if ok is None:
        die_usage(f"cannot read the {tool} range {spec!r} in gui/web/package.json engines (use ^X.Y.Z, >=X.Y.Z, ||)")
    if not ok:
        die_usage(f"{tool} {'.'.join(map(str, version))} is not supported; gui/web/package.json engines.{tool} "
                  f"is {spec!r} (Node: gui/web/.nvmrc)")


# --- running ------------------------------------------------------------------------------------


def group_alive(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def stop_group(process: subprocess.Popen) -> None:
    """Stop the step and everything it started (dev server, GUI server, browsers)."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True)
        return
    # SIGINT first: Playwright then stops the web servers it started (they run in their own process groups).
    for sig, seconds in ((signal.SIGINT, 20), (signal.SIGTERM, 5), (signal.SIGKILL, 0)):
        if not group_alive(process.pid):
            return
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and group_alive(process.pid):
            time.sleep(0.2)


def run_step(step: Step, env: dict[str, str], log: Path) -> None:
    step.log = rel(log)
    start = time.monotonic()
    # Vitest ignores FORCE_COLOR=0; Playwright warns about NO_COLOR next to its own FORCE_COLOR.
    if not step.name.startswith("e2e"):
        env = {**env, "NO_COLOR": "1"}
    group = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write("$ npm " + " ".join(step.command[1:]) + "\n")
        f.flush()
        process = subprocess.Popen(step.command, cwd=WEB, env=env, stdout=f, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, **group)
        try:
            step.code = process.wait()
        except BaseException:
            stop_group(process)
            raise
    step.seconds = time.monotonic() - start
    step.ok = step.code == 0
    lines = clean_lines(log.read_text(encoding="utf-8", errors="replace"))
    found = [ln.strip() for ln in lines if SUMMARY_LINES[step.name].search(ln)]
    step.summary = re.sub(r"\s*\|\s*|\s{2,}", " ", found[-1]).strip() if found else ""
    if step.name in NOTE_LINES:
        step.notes = [ln.strip() for ln in lines if NOTE_LINES[step.name].search(ln)][:3]
    if not step.ok:
        errors = [ln for ln in lines if ln.strip() and ERROR_LINES[step.name].search(ln)]
        step.errors = errors[:MAX_ERROR_LINES] or [ln for ln in lines if ln.strip()][-8:]


def on_sigterm(_signum, _frame) -> None:
    raise KeyboardInterrupt


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/verify-web.py",
        description="Run npm ci, the format check, lint, unit tests, the production build and the end-to-end "
        "tests of gui/web and print a short summary; the full output goes to build/logs/<tag>-*.log. Exit code 0 "
        "if every step passed, 1 if one failed, 2 on a usage error.",
        epilog="examples:\n"
        "  scripts/verify-web.py                  # everything, from a clean npm ci\n"
        "  scripts/verify-web.py --skip-install   # keep node_modules\n"
        "  scripts/verify-web.py --skip-e2e       # no browser, no GUI server\n"
        "  scripts/verify-web.py --tag web-ci     # logs as build/logs/web-ci-*.log\n"
        "  scripts/verify-web.py --no-wait        # do not wait for ninja, ctest or the VM",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--skip-install", action="store_true", help="do not run npm ci (use the node_modules there)")
    parser.add_argument("--skip-e2e", action="store_true", help="do not run the Playwright tests")
    parser.add_argument("--tag", default="web", help="prefix of the log names (default: web)")
    parser.add_argument("--json", action="store_true", help='print the summary as JSON ({"vehicle_sil_verify_web": 1, ...})')
    parser.add_argument("--no-wait", action="store_true",
                        help="do not wait while ninja, ctest or the Windows VM (qemu-system-x86*) runs")
    parser.add_argument("--wait-limit", type=float, default=DEFAULT_LIMIT_S / 60, metavar="MIN",
                        help="stop waiting for them after MIN minutes and go on with a warning (default: %(default)g)")
    args = parser.parse_args()
    if args.wait_limit < 0:
        die_usage("--wait-limit must be 0 or more minutes")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.tag):
        die_usage(f"--tag {args.tag!r}: use letters, digits, '.', '_' and '-' only (it becomes part of file names)")

    node, npm = shutil.which("node"), shutil.which("npm")
    if node is None or npm is None:
        die_usage("node and npm are not on PATH; install the Node version in gui/web/.nvmrc")
    engines = json.loads((WEB / "package.json").read_text(encoding="utf-8")).get("engines", {})
    for tool, path in (("node", node), ("npm", npm)):
        if tool in engines:
            check_engine(tool, path, engines[tool])
    if not args.skip_e2e and shutil.which("uv") is None:
        die_usage("uv is not on PATH; the e2e tests start the GUI server with uv (or pass --skip-e2e)")
    if args.skip_install and not (WEB / "node_modules").is_dir():
        die_usage("gui/web/node_modules is missing; run without --skip-install")

    steps = []
    if not args.skip_install:
        steps.append(Step("install", [npm, "ci", "--no-fund", "--no-audit"]))
    steps += [Step("format", [npm, "run", "format:check"]), Step("lint", [npm, "run", "lint"]),
              Step("test", [npm, "test"]), Step("build", [npm, "run", "build"])]
    if not args.skip_e2e:
        steps += [Step("e2e", [npm, "run", "e2e"]), Step("e2e-dev", [npm, "run", "e2e:dev"])]

    env = {**os.environ, "CI": "1", "NG_CLI_ANALYTICS": "false", "FORCE_COLOR": "0"}
    env.pop("NO_COLOR", None)  # Playwright warns when NO_COLOR and FORCE_COLOR are both set
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    logs = {s.name: LOG_DIR / f"{args.tag}-{s.name}.log" for s in steps}
    for stale in logs.values():
        stale.unlink(missing_ok=True)
    signal.signal(signal.SIGTERM, on_sigterm)

    done: list[Step] = []
    try:
        for step in steps:
            if step.name in HEAVY and not args.no_wait:
                wait_until_quiet("verify-web", args.wait_limit * 60)
            run_step(step, env, logs[step.name])
            done.append(step)
            if not args.json:
                head = f"web {step.name}: " + ("ok" if step.ok else f"FAILED (exit {step.code})")
                head += f" ({step.seconds:.0f} s)"
                if step.summary:
                    head += f" - {step.summary}"
                print(head)
                for ln in step.notes:
                    print("  note: " + ln)
                for ln in step.errors:
                    print("    " + ln)
                if not step.ok:
                    print(f"  log: {step.log}")
                sys.stdout.flush()
            if not step.ok:
                break
    except KeyboardInterrupt:
        print("verify-web: interrupted", file=sys.stderr)
        return 130

    failed = [s for s in done if not s.ok]
    if args.json:
        print(json.dumps({"vehicle_sil_verify_web": 1, "ok": not failed, "steps": [s.to_json() for s in done]},
                         indent=2, ensure_ascii=False))
    elif failed:
        print(f"verify-web: FAILED {failed[0].name}")
    else:
        print(f"verify-web: {len(done)} of {len(done)} steps passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
