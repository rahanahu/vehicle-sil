#!/usr/bin/env python3
"""Install, check, unit-test, build and end-to-end test the web front end (gui/web), with a short summary.

Steps, in order: npm ci, npm run format:check, npm run lint (gui/web, gui/client and gui/timeline), npm test (the Angular unit tests, in a
headless Chromium that Playwright provides; scripts/doctor.py checks that it is installed), npm run
test:client (the client library gui/client: a type check, then its unit tests: the library on Node without
Angular, the parts in a headless Chromium that Playwright provides; scripts/doctor.py checks that it is installed), npm run test:timeline (the timeline element gui/timeline: whether vehicle-sil-timeline.js is what its sources
make, then the unit tests of viewer.js on Node, no DOM),
i18n (npm run i18n:extract, then the
extracted messages are compared with each translation file gui/web/src/locale/messages.<lang>.json: a missing
message, a message not translated yet ("target": null), a changed source and a broken target fail; obsolete
entries are listed; see scripts/web_i18n.py), npm run build, npm run build:client (the
client library bundled into one ESM file, build/gui-client/vehicle-sil-gui-client.mjs, with the esbuild that the
Angular build tools bring), npm run pack:client (the client library as an npm package,
build/gui-client/vehicle-sil-gui-client-<version>.tgz, with its types; it is installed and used to check it), npm
run e2e (the built
app served by the GUI server of gui/server, which runs the stand-in launcher that replays recordings), and, with --e2e-dev, npm run e2e:dev (the same tests through the
dev server with its proxy to the GUI server). The full output of each step goes to build/logs/<tag>-<step>.log (tag: web) and the Playwright JUnit XML report of each e2e step to build/logs/<tag>-<step>.junit.xml; the
summary shows one line per step and, for a failed step, its first error lines and its log. Stops at the first
failed step. A stopped run (Ctrl-C, SIGTERM) stops the whole process group of the running step, so no dev
server or GUI server is left behind. --summarize-junit summarizes an existing JUnit XML file (overall counts,
per-file cumulative durations and the slowest individual tests).

Before each heavy step (unit tests, build, e2e) it waits while a C++ build or test run (ninja, ctest) or the
Windows VM (qemu-system-x86*) runs on this machine, because the browser tests are timing-sensitive and so are
the C++ tests (scripts/busy_machine.py). It gives up after --wait-limit minutes and goes on with a warning.

While working on the end-to-end tests, --e2e-grep, --e2e-repeat and --skip-checks narrow a run (they are
passed to Playwright as --grep and --repeat-each; --skip-checks leaves out the format check, lint, unit
tests and the i18n step, not the build). --i18n-only runs only the i18n step; --i18n-update rewrites the
translation files to match the extraction (new messages get "target": null, obsolete ones are dropped and
printed) and stops; the i18n step fails until each null is translated. --allow-untranslated lets the i18n step
pass with messages not translated yet, for work in progress on one's own machine (CI does not pass it).

Exit codes: 0 every step passed, 1 a step failed, 2 usage error (missing tools, unsupported Node or npm).
Requires Python 3.11+ (standard library only), Node and npm (gui/web/.nvmrc), and uv for the e2e steps.
On Windows, run it with a Windows Python (python.org or MinGW, where shutil.which finds npm.cmd).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import web_i18n
import job_summary
import verify_common
from busy_machine import DEFAULT_LIMIT_S, wait_until_quiet

ROOT = verify_common.ROOT
WEB = ROOT / "gui" / "web"
LOG_DIR = verify_common.LOG_DIR
EXTRACTION = ROOT / "build" / "i18n" / "messages.json"  # npm run i18n:extract
LOCALE_DIR = WEB / "src" / "locale"
LINE_CHARS = verify_common.LINE_CHARS
MAX_ERROR_LINES = 12

ANSI = verify_common.ANSI

# Lines worth showing from a failed step, by step.
E2E_ERRORS = re.compile(r"✘|^\s+\d+\) |Error:|Timed out|\d+ (failed|flaky)|webServer|WebServer.*(rror|exit)")
ERROR_LINES = {
    "install": re.compile(r"npm (error|ERR!)|EBADENGINE|ERESOLVE"),
    "install-workbench": re.compile(r"npm (error|ERR!)|EBADENGINE|ERESOLVE"),
    "format": re.compile(r"\[warn\]|\[error\]"),
    "lint": re.compile(r"^\S.*\.(ts|html)$|^\s+\d+:\d+\s+(error|warning)\s|problems?\s*\(|Error"),
    "test": re.compile(r"\bFAIL\b|×|AssertionError|Error:|^\s+(Test Files|Tests)\s"),
    "client": re.compile(r"\bFAIL\b|×|AssertionError|Error:|error TS\d+|^\s+(Test Files|Tests)\s"),
    "i18n": re.compile(r"\[ERROR\]|error TS\d+|^Error|rror:"),
    "build": re.compile(r"^\s*(✘|X)\s*\[ERROR\]|\[ERROR\]|error TS\d+|^Error|budget"),
    "bundle": re.compile(r"\[ERROR\]|^Error|rror:|Cannot find module"),
    "package": re.compile(r"pack-client: |error TS\d+|\[ERROR\]|rror:"),
    "e2e": E2E_ERRORS,
    "e2e-dev": E2E_ERRORS,
}

# A line that sums up a passed step.
E2E_SUMMARY = re.compile(r"^\s+\d+ passed")
SUMMARY_LINES = {
    "install": re.compile(r"^added \d+ packages"),
    "install-workbench": re.compile(r"^added \d+ packages"),
    "format": re.compile(r"All matched files use Prettier code style"),
    "lint": re.compile(r"All files pass linting"),
    "test": re.compile(r"^\s+Tests\s+\d+"),
    "client": re.compile(r"^\s+Tests\s+\d+"),
    "timeline": re.compile(r"^\S+ pass \d+\s*$"),  # node --test: "ℹ pass 17" (or "# pass 17" as TAP)
    "i18n": re.compile(r"Extraction Complete"),
    "build": re.compile(r"Initial total"),
    "bundle": re.compile(r"vehicle-sil-gui-client\.mjs\s+\S+"),
    "package": re.compile(r"pack-client: \S+\.tgz"),
    "e2e": E2E_SUMMARY,
    "e2e-dev": E2E_SUMMARY,
}

# The unit tests of gui/web and of the parts (gui/client/src/elements) run in Chromium; Playwright says this when it is not installed.
BROWSER_MISSING = re.compile(r"Executable doesn't exist|npx playwright install")
BROWSER_HINT = ("The browser of the unit tests is not installed. Install it with `npx playwright install "
                "--only-shell chromium` in gui/web (add --with-deps on a fresh Linux); scripts/doctor.py checks it.")

# Steps that wait while timing-sensitive work runs on the machine.
HEAVY = {"test", "client", "i18n", "build", "e2e", "e2e-dev"}

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
    junit: str = ""
    summary: str = ""
    notes: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        out = {"step": self.name, "ok": self.ok, "exit_code": self.code, "seconds": round(self.seconds, 1),
               "log": self.log, "summary": self.summary, "notes": self.notes, "errors": self.errors}
        if self.junit:
            out["junit"] = self.junit
        return out


def die_usage(message: str) -> None:
    verify_common.die_usage("verify-web", message)


def rel(path: Path) -> str:
    return verify_common.rel(path, ROOT)


def clean_lines(text: str) -> list[str]:
    return verify_common.clean_lines(text, LINE_CHARS)


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


stop_group = verify_common.stop_group


def run_step(step: Step, env: dict[str, str], log: Path, junit: Path | None = None) -> None:
    step.log = rel(log)
    start = time.monotonic()
    # Vitest ignores FORCE_COLOR=0; Playwright warns about NO_COLOR next to its own FORCE_COLOR.
    if not step.name.startswith("e2e"):
        env = {**env, "NO_COLOR": "1"}
    if junit is not None:
        env = {**env, "VEHICLE_SIL_PLAYWRIGHT_JUNIT": str(junit)}
    group = verify_common.process_group_kwargs()
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
    if junit is not None and junit.is_file():
        step.junit = rel(junit)
    lines = clean_lines(log.read_text(encoding="utf-8", errors="replace"))
    found = [ln.strip() for ln in lines if SUMMARY_LINES[step.name].search(ln)]
    step.summary = re.sub(r"\s*\|\s*|\s{2,}", " ", found[-1]).strip() if found else ""
    if step.name == "lint" and not step.ok:
        # "All files pass linting" is ng lint's; the lint of gui/client or gui/timeline may still have failed
        step.summary = ""
    if step.name in NOTE_LINES:
        step.notes = [ln.strip() for ln in lines if NOTE_LINES[step.name].search(ln)][:3]
    if not step.ok:
        errors = [ln for ln in lines if ln.strip() and ERROR_LINES[step.name].search(ln)]
        step.errors = errors[:MAX_ERROR_LINES] or [ln for ln in lines if ln.strip()][-8:]
        if any(BROWSER_MISSING.search(ln) for ln in lines):
            step.errors.insert(0, BROWSER_HINT)


def check_translations(step: Step, log: Path, allow_untranslated: bool) -> None:
    """After the extraction: compare it with each translation file; the step fails on missing, not translated yet
    (target null, unless allow_untranslated) or wrong ones (a changed source, a broken target)."""
    try:
        reports = web_i18n.check(EXTRACTION, LOCALE_DIR)
    except (OSError, ValueError) as e:
        step.ok, step.code, step.summary, step.errors = False, 1, "", [str(e)]
        return
    lines = []
    for r in reports:
        lines.append(r.summary())
        lines += [f"  {r.language} missing: {m}" for m in r.missing]
        lines += [f"  {r.language} wrong: {m}" for m in r.problems]
        lines += [f"  {r.language} not translated yet: {m}" for m in r.untranslated]
        lines += [f"  {r.language} obsolete: {m}" for m in r.obsolete]
    with open(log, "a", encoding="utf-8") as f:
        f.write("\n".join(["", "$ compare with gui/web/src/locale/messages.*.json", *lines]) + "\n")
    step.summary = "; ".join(r.summary() for r in reports) if reports else "no translation files"
    step.notes = [f"{r.language} obsolete: {m}" for r in reports for m in r.obsolete][:5]
    if not all(r.passes(allow_untranslated) for r in reports):
        step.ok, step.code = False, 1
        step.errors = [f"{r.language} {kind}: {m}" for r in reports
                       for kind, items in (("missing", r.missing), ("wrong", r.problems),
                                           ("not translated yet", [] if allow_untranslated else r.untranslated))
                       for m in items]
        step.errors = step.errors[:MAX_ERROR_LINES]
        if any(r.missing or any("source differs" in m for m in r.problems) for r in reports):
            step.errors.append("add the new messages with scripts/verify-web.py --i18n-update; they come in as "
                               "\"target\": null, and the step fails until each target is translated in "
                               "gui/web/src/locale/messages.<lang>.json")
        elif not allow_untranslated and any(r.untranslated for r in reports):
            step.errors.append("translate each \"target\": null in gui/web/src/locale/messages.<lang>.json (to "
                               "go on without translating on your own machine, pass --allow-untranslated)")
        if any(r.problems for r in reports):
            step.errors.append("fix a wrong target in gui/web/src/locale/messages.<lang>.json")


def update_translations(npm: str, env: dict[str, str]) -> int:
    step = Step("i18n", [npm, "run", "i18n:extract"])
    log = LOG_DIR / "i18n-update.log"
    run_step(step, env, log)
    if not step.ok:
        print(f"verify-web --i18n-update: the extraction FAILED (exit {step.code}), log: {step.log}")
        for ln in step.errors:
            print("    " + ln)
        return 1
    for language, added, dropped in web_i18n.update(EXTRACTION, LOCALE_DIR):
        hint = "; translate them, the i18n step fails on null" if added else ""
        print(f"{language}: {added} added as not translated yet (\"target\": null{hint}), "
              f"{len(dropped)} obsolete dropped")
        for ln in dropped:
            print("  dropped: " + ln)
    return 0


def summarize_junit(path: Path, top: int, as_json: bool) -> int:
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as e:
        die_usage(f"cannot read JUnit XML {path}: {e}")
    root = tree.getroot()
    if root.tag not in ("testsuites", "testsuite"):
        die_usage(f"{path}: expected <testsuites> or <testsuite> root element, got <{root.tag}>")
    suites = list(root.iter("testsuite")) or [root]

    cases: list[dict] = []
    by_file: dict[str, dict] = {}
    for suite in suites:
        suite_file = suite.get("file") or suite.get("name") or ""
        for case in suite.findall("testcase"):
            name = case.get("name") or "(unnamed)"
            file_name = case.get("file") or case.get("classname") or suite_file or "(unknown)"
            raw_time = case.get("time")
            try:
                seconds = float(raw_time) if raw_time not in (None, "") else 0.0
            except ValueError:
                die_usage(f"{path}: invalid testcase time {raw_time!r} on {name!r}")
            if not math.isfinite(seconds) or seconds < 0:
                die_usage(f"{path}: invalid testcase time {raw_time!r} on {name!r}")
            if case.find("failure") is not None or case.find("error") is not None:
                status = "failed"
            elif case.find("skipped") is not None or case.get("status") == "skipped":
                status = "skipped"
            else:
                status = "passed"
            cases.append({"file": file_name, "name": name, "status": status, "seconds": seconds})
            entry = by_file.setdefault(
                file_name,
                {"file": file_name, "tests": 0, "passed": 0, "failed": 0, "skipped": 0,
                 "seconds": 0.0, "max_seconds": 0.0},
            )
            entry["tests"] += 1
            entry[status] += 1
            entry["seconds"] += seconds
            if seconds > entry["max_seconds"]:
                entry["max_seconds"] = seconds

    total = len(cases)
    passed = sum(1 for c in cases if c["status"] == "passed")
    failed = sum(1 for c in cases if c["status"] == "failed")
    skipped = sum(1 for c in cases if c["status"] == "skipped")
    total_seconds = sum(c["seconds"] for c in cases)
    files_sorted = sorted(by_file.values(), key=lambda f: (-f["seconds"], f["file"]))
    slowest = sorted(cases, key=lambda c: (-c["seconds"], c["file"], c["name"]))[:top]
    ok = failed == 0

    if as_json:
        out = {
            "vehicle_sil_verify_web_junit": 1,
            "path": rel(path),
            "ok": ok,
            "total": total,
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "seconds": round(total_seconds, 3),
            "files": [
                {
                    "file": f["file"],
                    "tests": f["tests"],
                    "passed": f["passed"],
                    "failed": f["failed"],
                    "skipped": f["skipped"],
                    "seconds": round(f["seconds"], 3),
                    "max_seconds": round(f["max_seconds"], 3),
                }
                for f in files_sorted
            ],
            "slowest": [
                {
                    "file": c["file"],
                    "name": c["name"],
                    "status": c["status"],
                    "seconds": round(c["seconds"], 3),
                }
                for c in slowest
            ],
        }
        print(json.dumps(out, indent=2, ensure_ascii=False))
        return 0 if ok else 1

    state = "ok" if ok else "FAILED"
    print(f"verify-web junit: {rel(path)} - {state} - {passed} passed, {failed} failed, "
          f"{skipped} skipped, {total} total ({total_seconds:.1f} s)")
    print(f"files ({len(files_sorted)}):")
    for f in files_sorted:
        print(f"  {f['seconds']:7.2f} s  max {f['max_seconds']:6.2f} s  {f['tests']:3d} tests "
              f"({f['passed']} passed, {f['failed']} failed, {f['skipped']} skipped)  {f['file']}")
    print(f"slowest tests (top {len(slowest)} of {total}):")
    for c in slowest:
        print(f"  {c['seconds']:7.2f} s  [{c['status']}]  {c['file']} :: {c['name']}")
    return 0 if ok else 1


def main() -> int:
    verify_common.reconfigure_stdio(encoding="utf-8", stderr=True)
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
        "  scripts/verify-web.py --no-wait        # do not wait for ninja, ctest or the VM\n"
        "  scripts/verify-web.py --skip-install --skip-checks --e2e-grep 'double click' --e2e-repeat 5\n"
        "  scripts/verify-web.py --i18n-only      # only compare the messages with the translation files\n"
        "  scripts/verify-web.py --i18n-update    # add new messages to the files as not translated yet\n"
        "  scripts/verify-web.py --i18n-only --allow-untranslated   # work in progress: let null targets pass\n"
        "  scripts/verify-web.py --summarize-junit build/logs/web-e2e.junit.xml",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--summarize-junit", metavar="PATH",
                        help="run nothing; summarize a Playwright JUnit XML file (per-file cumulative time and slowest tests)")
    parser.add_argument("--top", type=int, default=20, metavar="N",
                        help="number of slowest tests to list with --summarize-junit (default: %(default)s)")
    parser.add_argument("--skip-install", action="store_true", help="do not run npm ci (use the node_modules there)")
    parser.add_argument("--skip-e2e", action="store_true", help="do not run the Playwright tests")
    parser.add_argument("--e2e-dev", action="store_true",
                        help="also run the Playwright tests through the dev server (npm run e2e:dev); "
                             "they repeat the e2e tests, so by default only the built app is tested")
    parser.add_argument("--tag", default="web", help="prefix of the log names (default: web)")
    parser.add_argument("--json", action="store_true", help='print the summary as JSON ({"vehicle_sil_verify_web": 1, ...})')
    parser.add_argument("--no-wait", action="store_true",
                        help="do not wait while ninja, ctest or the Windows VM (qemu-system-x86*) runs")
    parser.add_argument("--wait-limit", type=float, default=DEFAULT_LIMIT_S / 60, metavar="MIN",
                        help="stop waiting for them after MIN minutes and go on with a warning (default: %(default)g)")
    parser.add_argument("--skip-checks", action="store_true",
                        help="do not run the format check, lint, unit tests (gui/web, gui/client and gui/timeline) and "
                        "the i18n step (the build and e2e still run)")
    parser.add_argument("--workbench", action="store_true", help="build the alternate GUI and run its browser test")
    parser.add_argument("--e2e-grep", metavar="PATTERN", help="run only the e2e tests whose title matches PATTERN")
    parser.add_argument("--e2e-repeat", type=int, default=1, metavar="N", help="run each e2e test N times")
    parser.add_argument("--i18n-only", action="store_true",
                        help="run only the i18n step (extraction and comparison with the translation files)")
    parser.add_argument("--i18n-update", action="store_true",
                        help="rewrite the translation files to match the extraction (new messages get "
                        "\"target\": null, obsolete ones are dropped) and stop; needs node_modules")
    parser.add_argument("--allow-untranslated", action="store_true",
                        help="let the i18n step pass with messages marked not translated yet (\"target\": null); "
                        "for work in progress on your own machine, CI does not pass it")
    args = parser.parse_args()
    if args.top < 1:
        die_usage("--top must be 1 or more")
    if args.summarize_junit is not None:
        if not args.summarize_junit.strip():
            die_usage("--summarize-junit requires a file path")
        return summarize_junit(Path(args.summarize_junit), args.top, args.json)
    if args.e2e_repeat < 1:
        die_usage("--e2e-repeat must be 1 or more")
    if args.wait_limit < 0:
        die_usage("--wait-limit must be 0 or more minutes")
    verify_common.validate_tag("verify-web", args.tag)

    node, npm = shutil.which("node"), shutil.which("npm")
    if node is None or npm is None:
        die_usage("node and npm are not on PATH; install the Node version in gui/web/.nvmrc")
    engines = json.loads((WEB / "package.json").read_text(encoding="utf-8")).get("engines", {})
    for tool, path in (("node", node), ("npm", npm)):
        if tool in engines:
            check_engine(tool, path, engines[tool])
    i18n_alone = args.i18n_only or args.i18n_update
    if args.workbench and i18n_alone:
        die_usage("--workbench cannot be used with the i18n-only options")
    if not i18n_alone and not args.skip_e2e and shutil.which("uv") is None:
        die_usage("uv is not on PATH; the e2e tests start the GUI server with uv (or pass --skip-e2e)")
    if (args.skip_install or i18n_alone) and not (WEB / "node_modules").is_dir():
        die_usage("gui/web/node_modules is missing; run npm ci in gui/web (or verify-web.py without --skip-install)")

    steps = []
    if args.i18n_only:
        steps.append(Step("i18n", [npm, "run", "i18n:extract"]))
    elif args.i18n_update:
        pass
    elif not args.skip_install:
        steps.append(Step("install", [npm, "ci", "--no-fund", "--no-audit"]))
    if args.workbench:
        if not args.skip_install:
            steps.append(Step("install-workbench", [npm, "--prefix", "workbench", "ci", "--no-fund", "--no-audit"]))
        steps.append(Step("build", [npm, "run", "build:workbench"]))
    if not i18n_alone and not args.workbench and not args.skip_checks:
        steps += [Step("format", [npm, "run", "format:check"]), Step("lint", [npm, "run", "lint"]),
                  Step("test", [npm, "test"]), Step("client", [npm, "run", "test:client"]),
                  Step("timeline", [npm, "run", "test:timeline"]), Step("i18n", [npm, "run", "i18n:extract"])]
    if not i18n_alone and not args.workbench:
        steps += [Step("build", [npm, "run", "build"]), Step("bundle", [npm, "run", "build:client"]),
                  Step("package", [npm, "run", "pack:client"])]
    if not i18n_alone and not args.skip_e2e:
        grep = args.e2e_grep or ("workbench" if args.workbench else None)
        narrow = (["--grep", grep] if grep else []) + \
            (["--repeat-each", str(args.e2e_repeat)] if args.e2e_repeat > 1 else [])
        extra = ["--", *narrow] if narrow else []
        steps += [Step("e2e", [npm, "run", "e2e", *extra])]
        if args.e2e_dev:
            steps += [Step("e2e-dev", [npm, "run", "e2e:dev", *extra])]

    env = {**os.environ, "CI": "1", "NG_CLI_ANALYTICS": "false", "FORCE_COLOR": "0"}
    if args.workbench:
        env["VEHICLE_SIL_VERIFY_WORKBENCH"] = "1"
    env.pop("NO_COLOR", None)  # Playwright warns when NO_COLOR and FORCE_COLOR are both set
    env.pop("VEHICLE_SIL_PLAYWRIGHT_JUNIT", None)
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    logs = {s.name: LOG_DIR / f"{args.tag}-{s.name}.log" for s in steps}
    junits = {s.name: LOG_DIR / f"{args.tag}-{s.name}.junit.xml" for s in steps if s.name in ("e2e", "e2e-dev")}
    for stale in [*logs.values(), *junits.values()]:
        stale.unlink(missing_ok=True)
    verify_common.install_sigterm_handler()
    if args.i18n_update:
        try:
            return update_translations(npm, env)
        except KeyboardInterrupt:
            print("verify-web: interrupted", file=sys.stderr)
            return 130

    done: list[Step] = []
    try:
        for step in steps:
            if step.name in HEAVY and not args.no_wait:
                wait_until_quiet("verify-web", args.wait_limit * 60)
            run_step(step, env, logs[step.name], junits.get(step.name))
            if step.name == "i18n" and step.ok:
                check_translations(step, logs[step.name], args.allow_untranslated)
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
    sys.exit(job_summary.run(main))
