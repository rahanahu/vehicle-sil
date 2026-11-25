#!/usr/bin/env python3
"""Check the browser GUI as a whole: the server's tests, then the screen (scripts/verify-web.py), with a short summary.

Steps, in order:

  sync    uv sync --locked (fails when uv.lock does not match the pyproject.toml files)
  server  uv run pytest gui/server/tests: the GUI server against the stand-in launcher
          (gui/server/tests/stub_launcher.py) (API, WebSocket, security,
          stopping, and the comparison of gui/protocol/examples); JUnit XML in build/logs/<tag>-server.xml
  web     scripts/verify-web.py --json: npm ci, format, lint, unit tests, build and e2e of gui/web and gui/client; its steps
          are listed under this one, with their logs
  example the example clients in examples/gui_client (Node 22 or later) against the GUI server and the stand-in
          launcher: run.mjs (plain JavaScript, no library) and run-with-library.mjs (the bundled client library from
          the web step): a run that passes, runs that fail, a refused token; and variables.mjs (the library again:
          write a variable of an ECU before the first step, step, check what is read while the simulation stands
          still) against the stand-in launcher with its stand-in for the control server and, with a real launcher,
          against that one and the ecu_main example ECUs (gui/server/tests/test_example_client.py, which the
          server step leaves out); not with --skip-web or -k
  real    with a real launcher only (--launcher <path>, or VEHICLE_SIL_GUI_LAUNCHER): npm run e2e:real, the
          screen, the GUI server, that vehicle_sil_launch and the ecu_main example ECUs built next to it (pass,
          a participant left out, stop, failing checks, a failing run); needs the build of gui/web (the web step,
          or an earlier one); JUnit XML in build/logs/<tag>-real.junit.xml. Without a launcher it prints "skipped: no launcher" and does not fail

--skip-server leaves out the server step only (CI runs it in a job of its own, at the same time as the others).

With a real launcher, the server step also runs gui/server/tests/test_real_launcher.py, which checks the Events v4
contract against the real launcher, and test_real_variables.py, which reads and writes the
variables of the example ECUs through the GUI server (a launcher built with its control server); without one,
those tests are skipped and say why.

The full output of each step goes to build/logs/<tag>-<step>.log (the web steps: build/logs/<tag>-web-*.log).
The summary shows one line per step and, for a failed step, its first error lines and its log. Stops at the
first failed step. A stopped run (Ctrl-C, SIGTERM) stops the whole process group of the running step.

Before the server, example and real steps it waits while a C++ build or test run (ninja, ninja-build, ctest) or
the Windows VM (qemu-system-x86*) runs on this machine, because these tests are timing-sensitive and so are the
C++ tests (scripts/busy_machine.py; verify-web.py waits before each of its own heavy steps). It gives up after
--wait-limit minutes and goes on with a warning.

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
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import job_summary
import verify_common
from busy_machine import DEFAULT_LIMIT_S, wait_until_quiet

ROOT = verify_common.ROOT
LOG_DIR = verify_common.LOG_DIR
LINE_CHARS = verify_common.LINE_CHARS
MAX_ERROR_LINES = 12
ANSI = verify_common.ANSI

ERROR_LINES = {
    "sync": re.compile(r"error|lockfile|needs to be updated", re.IGNORECASE),
    "server": re.compile(r"^(FAILED|ERROR) |^E\s+|Error:|^\S+\.py:\d+: "),
    "example": re.compile(r"^(FAILED|ERROR) |^E\s+|Error:|^\S+\.py:\d+: "),
    "web": re.compile(r"."),
    "real": re.compile(r"✘|^\s+\d+\) |Error:|Timed out|\d+ (failed|flaky)"),
}
SUMMARY_LINES = {
    "sync": re.compile(r"^(Resolved|Audited|Installed|Checked) "),
    "server": re.compile(r"^=*\s*\d+ (passed|failed)|^\d+ (passed|failed)"),
    "example": re.compile(r"^=*\s*\d+ (passed|failed|skipped)|^\d+ (passed|failed|skipped)"),
    "web": re.compile(r"$^"),
    "real": re.compile(r"^\s+\d+ passed"),
}


@dataclass
class Step:
    name: str
    command: list[str]
    ok: bool = True
    code: int = 0
    seconds: float = 0.0
    log: str = ""
    junit: str = ""
    summary: str = ""
    errors: list[str] = field(default_factory=list)
    substeps: list[dict] = field(default_factory=list)

    def to_json(self) -> dict:
        out = {"step": self.name, "ok": self.ok, "exit_code": self.code, "seconds": round(self.seconds, 1),
               "log": self.log, "summary": self.summary, "errors": self.errors}
        if self.junit:
            out["junit"] = self.junit
        if self.substeps:
            out["steps"] = self.substeps
        return out


def die_usage(message: str) -> None:
    verify_common.die_usage("verify-gui", message)


def rel(path: Path) -> str:
    return verify_common.rel(path, ROOT)


def clean_lines(text: str) -> list[str]:
    return verify_common.clean_lines(text, LINE_CHARS)


def stale_launcher_source(launcher: Path, root: Path | None = None) -> Path | None:
    """Return the newest source file under libs/ or tools/launch/ newer than `launcher`, or None."""
    base_root = ROOT if root is None else root
    try:
        newest_mtime = launcher.stat().st_mtime
    except OSError:
        return None
    newest_path: Path | None = None
    for base in (base_root / "libs", base_root / "tools" / "launch"):
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            try:
                if not path.is_file():
                    continue
                mtime = path.stat().st_mtime
            except OSError:
                continue
            if mtime > newest_mtime:
                newest_mtime = mtime
                newest_path = path
    return newest_path


stop_group = verify_common.stop_group


def run_step(step: Step, env: dict[str, str], log: Path, junit: Path | None = None) -> None:
    step.log = rel(log)
    start = time.monotonic()
    if junit is not None:
        env = {**env, "VEHICLE_SIL_PLAYWRIGHT_JUNIT": str(junit)}
    group = verify_common.process_group_kwargs()
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
    if junit is not None and junit.is_file():
        step.junit = rel(junit)
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
            step.errors.append("no test was run: check the -k expression or TEST targets")


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


def main() -> int:
    verify_common.reconfigure_stdio()
    parser = argparse.ArgumentParser(
        prog="scripts/verify-gui.py",
        description="Run the GUI server's tests and then scripts/verify-web.py, and print a short summary; the full "
        "output goes to build/logs/<tag>-*.log. Exit code 0 if every step passed, 1 if one failed, 2 on a usage error.",
        epilog="examples:\n"
        "  scripts/verify-gui.py                    # everything\n"
        "  scripts/verify-gui.py --skip-web         # only the server (no Node needed)\n"
        "  scripts/verify-gui.py --skip-server      # only the screen and the examples\n"
        "  scripts/verify-gui.py --no-sync gui/server/tests/test_availability.py\n"
        "                                           # only one server test file\n"
        "  scripts/verify-gui.py -k security        # only the server tests whose names match\n"
        "  scripts/verify-gui.py --skip-install --skip-e2e   # passed on to verify-web.py\n"
        "  scripts/verify-gui.py --no-wait          # do not wait for ninja, ctest or the VM\n"
        "  scripts/verify-gui.py --launcher build/linux-gcc-release/tools/launch/vehicle_sil_launch\n"
        "                                           # also against the real launcher and the example ECUs\n"
        "  scripts/verify-gui.py --skip-server --skip-web --no-sync --launcher <path> --real-spec e2e/real/scenario.spec.ts\n"
        "                                           # only one real-launcher E2E spec",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("targets", nargs="*", metavar="TEST",
                        help="specific server pytest files or nodeids (implies server tests only)")
    parser.add_argument("--skip-web", action="store_true", help="do not run scripts/verify-web.py")
    parser.add_argument("--skip-server", action="store_true",
                        help="do not run the server step (CI runs it in a job of its own, at the same time); the "
                        "web, example and real steps still run; not with -k or TEST targets (with --skip-web, "
                        "requires --launcher without --skip-e2e)")
    parser.add_argument("--launcher", metavar="PATH",
                        help="a real vehicle_sil_launch (build/<preset>/tools/launch/vehicle_sil_launch, with the "
                        "ecu_main example built next to it): also run the tests against it (default: "
                        "VEHICLE_SIL_GUI_LAUNCHER; without either they are skipped)")
    parser.add_argument("--skip-install", action="store_true", help="passed on to verify-web.py (keep node_modules)")
    parser.add_argument("--skip-e2e", action="store_true", help="passed on to verify-web.py (no browser tests)")
    parser.add_argument("--e2e-dev", action="store_true",
                        help="passed on to verify-web.py (also the browser tests through the dev server)")
    parser.add_argument("--real-spec", action="append", metavar="PATH",
                        help="run only this Playwright spec file in the real step (repeatable, e.g. e2e/real/scenario.spec.ts)")
    parser.add_argument("--real-grep", metavar="PATTERN",
                        help="run only the real-step e2e tests whose title matches PATTERN")
    parser.add_argument("--no-sync", action="store_true",
                        help="do not run uv sync --locked (use the existing .venv)")
    parser.add_argument("-k", dest="keyword", help="only the server tests matching this pytest -k expression")
    parser.add_argument("-n", "--workers", type=int, metavar="N",
                        help="pytest-xdist workers for the server step (default: up to 4 on a full run, serial with "
                        "-k or TEST targets; 1 disables xdist)")
    parser.add_argument("--tag", default="gui", help="prefix of the log names (default: gui)")
    parser.add_argument("--json", action="store_true",
                        help='print the summary as JSON ({"vehicle_sil_verify_gui": 1, "ok": ..., "steps": [...]})')
    parser.add_argument("--no-wait", action="store_true",
                        help="do not wait while ninja, ninja-build, ctest or the Windows VM (qemu-system-x86*) runs")
    parser.add_argument("--wait-limit", type=float, default=DEFAULT_LIMIT_S / 60, metavar="MIN",
                        help="stop waiting for them after MIN minutes and go on with a warning (default: %(default)g)")
    args = parser.parse_args()
    if args.wait_limit < 0:
        die_usage("--wait-limit must be 0 or more minutes")
    if args.workers is not None and args.workers < 1:
        die_usage("--workers must be 1 or more")
    real_specs: list[str] = []
    for raw_spec in args.real_spec or []:
        spec = raw_spec.strip()
        if not spec or spec.startswith("-"):
            die_usage(f"--real-spec {raw_spec!r}: give a Playwright spec path, not an option or empty value")
        real_specs.append(spec)
    server_only = bool(args.keyword or args.targets)
    launcher = args.launcher or os.environ.get("VEHICLE_SIL_GUI_LAUNCHER") or None
    if args.skip_server and server_only:
        die_usage("--skip-server leaves out the server step: cannot be combined with -k or TEST targets")
    if args.skip_server and args.skip_web and (launcher is None or args.skip_e2e):
        die_usage("--skip-server with --skip-web leaves nothing to run (pass --launcher <path> without --skip-e2e to run real E2E only)")
    verify_common.validate_tag("verify-gui", args.tag)
    uv = shutil.which("uv")
    if uv is None:
        die_usage("uv is not on PATH: install it (https://docs.astral.sh/uv/), then run this again")

    if launcher is not None:
        launcher_path = Path(launcher)
        if not launcher_path.is_file():
            die_usage(f"--launcher {launcher}: no such file (build/<preset>/tools/launch/vehicle_sil_launch)")
        stale = stale_launcher_source(launcher_path)
        if stale is not None:
            print(
                f"verify-gui: warning: launcher {launcher} is older than {rel(stale)}; "
                "rebuild it before trusting real-launcher tests",
                file=sys.stderr,
                flush=True,
            )
    if args.no_sync:
        venv = Path(os.environ.get("UV_PROJECT_ENVIRONMENT") or ROOT / ".venv")
        if not venv.is_dir():
            die_usage(f"--no-sync: virtual environment {venv} does not exist (run uv sync --locked first, or omit --no-sync)")
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    junit = LOG_DIR / f"{args.tag}-server.xml"
    example_tests = "gui/server/tests/test_example_client.py"
    uv_run_flag = "--no-sync" if args.no_sync else "--locked"
    pytest_targets = args.targets or ["gui/server/tests"]
    ignore_args = [] if args.targets else [f"--ignore={example_tests}"]
    pytest = [uv, "run", uv_run_flag, "pytest", *pytest_targets, "-q", "-rs", *ignore_args,
              f"--junitxml={junit}"]
    if args.keyword:
        pytest += ["-k", args.keyword]
    if args.workers is not None:
        if args.workers > 1:
            pytest += ["-n", str(args.workers)]
    elif not server_only:
        pytest += ["-n", str(min(os.cpu_count() or 1, 4))]
    steps = [] if args.no_sync else [Step("sync", [uv, "sync", "--locked"])]
    skipped_server = "--skip-server" if args.skip_server else None
    if skipped_server is None:
        steps.append(Step("server", pytest))
    server_only_reason = (
        "-k / targets (only server tests)" if args.keyword and args.targets
        else "targets (only server tests)" if args.targets
        else "-k (only server tests)"
    )
    skipped_web = "--skip-web" if args.skip_web else (server_only_reason if server_only else None)
    skipped_example = "--skip-web" if args.skip_web else (server_only_reason if server_only else None)
    if skipped_example is None:
        if shutil.which("node") is None:
            die_usage("node is not on PATH: the example step runs examples/gui_client/run.mjs (Node 22 or later; "
                      "or pass --skip-web)")
    if skipped_web is None:
        web = [sys.executable, str(ROOT / "scripts" / "verify-web.py"), "--json", "--tag", f"{args.tag}-web"]
        web += ["--skip-install"] * args.skip_install + ["--skip-e2e"] * args.skip_e2e + ["--e2e-dev"] * args.e2e_dev
        web += ["--no-wait"] if args.no_wait else ["--wait-limit", f"{args.wait_limit:g}"]
        steps.append(Step("web", web))
    if skipped_example is None:  # after web: it uses the library bundled there
        steps.append(Step("example", [uv, "run", uv_run_flag, "pytest", example_tests, "-q", "-rs"]))
    skipped_real = None
    if launcher is None:
        skipped_real = "no launcher (--launcher <path> or VEHICLE_SIL_GUI_LAUNCHER)"
    elif args.skip_e2e:
        skipped_real = "--skip-e2e"
    elif server_only:
        skipped_real = server_only_reason
    else:
        npm = shutil.which("npm")
        if npm is None:
            die_usage("npm is not on PATH: the real step runs the browser tests (gui/web/README.md)")
        real_narrow = [*real_specs, *(["--grep", args.real_grep] if args.real_grep else [])]
        real_extra = ["--", *real_narrow] if real_narrow else []
        steps.append(Step("real", [npm, "--prefix", "gui/web", "run", "e2e:real", *real_extra]))

    # The tests of the ECUs' variables need grpcio-tools (uv sync installs it): here they fail without it, not skip.
    # Sync runs first (or --no-sync checked .venv), so Playwright's uv run calls can skip redundant syncs.
    env = {
        **os.environ,
        "NO_COLOR": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "VEHICLE_SIL_GUI_REQUIRE_CONTROL": "1",
        "VEHICLE_SIL_GUI_UV_NO_SYNC": "1",
    }
    env.pop("VEHICLE_SIL_GUI_LAUNCHER", None)
    env.pop("VEHICLE_SIL_PLAYWRIGHT_JUNIT", None)
    if launcher is not None:
        env["VEHICLE_SIL_GUI_LAUNCHER"] = str(Path(launcher).resolve())
    env.pop("VIRTUAL_ENV", None)
    real_junit = LOG_DIR / f"{args.tag}-real.junit.xml"
    logs = {s.name: LOG_DIR / f"{args.tag}-{s.name}.log" for s in steps}
    for stale in [*logs.values(), junit, real_junit]:
        stale.unlink(missing_ok=True)
    verify_common.install_sigterm_handler()

    done: list[Step] = []
    try:
        for step in steps:
            if step.name not in ("sync", "web") and not args.no_wait:
                wait_until_quiet("verify-gui", args.wait_limit * 60)
            run_step(step, env, logs[step.name], real_junit if step.name == "real" else None)
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
    skipped = [(name, reason) for name, reason in (("server", skipped_server), ("web", skipped_web),
                                                   ("example", skipped_example), ("real", skipped_real))
               if reason is not None]
    if not failed and not args.json:
        for name, reason in skipped:
            print(f"gui {name}: skipped: {reason}")
    if args.json:
        out = {"vehicle_sil_verify_gui": 1, "ok": not failed, "steps": [s.to_json() for s in done]}
        if skipped:
            out["skipped"] = [{"step": name, "reason": reason} for name, reason in skipped]
        print(json.dumps(out, indent=2, ensure_ascii=False))
    elif failed:
        print(f"verify-gui: FAILED {failed[0].name}")
    else:
        print(f"verify-gui: {len(done)} of {len(done)} steps passed")
    if failed and failed[0].name == "web" and failed[0].code == 2:
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(job_summary.run(main))
