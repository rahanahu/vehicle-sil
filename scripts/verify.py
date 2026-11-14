#!/usr/bin/env python3
"""Configure, build and test CMake presets, and print a short summary.

The full output of every step goes to build/logs/<tag>-<preset>-{configure,build,test}.log and the
test results to build/logs/<tag>-<preset>.junit.xml. The summary names the failed stage, the failed
tests with the first lines of their output, the first compiler errors and the log files.

Exit codes: 0 every preset passed, 1 a preset failed, 2 usage error.
Requires Python 3.11+ (standard library only); runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import job_summary

ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = ROOT / "build" / "logs"
LINUX_PRESETS = ["linux-gcc-debug", "linux-gcc-release", "linux-clang-debug", "linux-clang-asan", "linux-clang-tsan"]
MAX_FAILED_NAMES = 20
MAX_FAILED_OUTPUTS = 3
OUTPUT_LINES = 15
# The longest line shown (longer ones are cut): a test's message whole, as far as it is sensible.
LINE_CHARS = 400
MAX_ERRORS = 3
ERROR_CONTEXT = 3

# ctest's status line of one test, e.g. "  12/831 Test #823: name ....***Failed  0.00 sec".
STATUS_LINE = re.compile(r"^\s*(?:\d+/\d+\s+)?Test\s+#\d+:\s+(\S+)\s+\.*\s*(\**)(.*?)\s+[\d.]+\s+sec\s*$")
START_LINE = re.compile(r"^\s+Start\s+\d+:\s")
# CTest 4.4 and later leave out ", 0 tests failed" when none failed ("100% tests passed out of 12").
SUMMARY_LINE = re.compile(r"^\d+% tests passed(?:, (\d+) tests? failed)? out of (\d+)")
LISTED_TEST = re.compile(r"^\s*\d+ - (\S+) \(([^)]*)\)")
BUILD_ERROR = re.compile(r"(^|[\s:])(fatal )?error:|undefined reference|ld returned|^CMake Error|^FAILED: ", re.IGNORECASE)


def die_usage(message: str) -> None:
    print(f"verify: error: {message}", file=sys.stderr)
    sys.exit(2)


def read_text(path: Path) -> str:
    """Reads a log as text: UTF-8, or UTF-16 when it has a BOM or NUL bytes (logs from Windows)."""
    data = path.read_bytes()
    if data[:2] in (b"\xff\xfe", b"\xfe\xff") or b"\0" in data[:200]:
        return data.decode("utf-16", "replace").replace("\r", "")
    return data.decode("utf-8", "replace").replace("\r", "")


@dataclass
class Result:
    name: str
    kind: str  # "preset" or "log"
    ok: bool = True
    stage: str | None = None  # the stage that failed
    total: int | None = None
    failed_tests: list[tuple[str, str]] = field(default_factory=list)  # (name, status)
    skipped_tests: list[str] = field(default_factory=list)
    outputs: dict[str, list[str]] = field(default_factory=dict)  # first lines of failed tests' output
    errors: list[list[str]] = field(default_factory=list)  # configure or build errors, with context
    note: str | None = None
    seconds: dict[str, float] = field(default_factory=dict)
    logs: dict[str, str] = field(default_factory=dict)

    def counts(self) -> str:
        if self.total is None:
            return ""
        failed, skipped = len(self.failed_tests), len(self.skipped_tests)
        parts = []
        if failed:
            parts.append(f"{failed} failed")
        parts.append(f"{self.total - failed - skipped} passed")
        if skipped:
            parts.append(f"{skipped} skipped")
        return ", ".join(parts)

    def to_json(self) -> dict:
        return {
            "name": self.name,
            "kind": self.kind,
            "ok": self.ok,
            "stage": self.stage,
            "total": self.total,
            "failed": [{"name": n, "status": s, "output": self.outputs.get(n, [])} for n, s in self.failed_tests],
            "skipped": self.skipped_tests,
            "errors": self.errors,
            "note": self.note,
            "seconds": {k: round(v, 1) for k, v in self.seconds.items()},
            "logs": self.logs,
        }


# --- reading ctest and build output -------------------------------------------------------------


def parse_ctest(text: str, result: Result) -> bool:
    """Fills the test results from ctest's output. False if the output has no final summary."""
    lines = text.splitlines()
    summary = None
    for i, line in enumerate(lines):
        m = SUMMARY_LINE.match(line)
        if m:
            summary = i
            result.total = int(m.group(2))
    if summary is None:
        return False
    section = None
    for line in lines[summary + 1:]:
        if line.startswith("The following tests FAILED"):
            section = "failed"
        elif line.startswith("The following tests did not run"):
            section = "skipped"
        elif section:
            m = LISTED_TEST.match(line)
            if not m:
                continue
            if section == "failed":
                result.failed_tests.append((m.group(1), m.group(2)))
            elif m.group(2) == "Skipped":
                result.skipped_tests.append(m.group(1))
    return True


def outputs_from_log(text: str, names: list[str]) -> dict[str, list[str]]:
    """The output ctest printed after the first failing status line of each named test."""
    wanted, found = set(names), {}
    current: str | None = None
    for line in text.splitlines():
        m = STATUS_LINE.match(line)
        if m or START_LINE.match(line) or SUMMARY_LINE.match(line):
            current = None
            if m and m.group(2) and m.group(1) in wanted and m.group(1) not in found:
                current = m.group(1)
                found[current] = []
            continue
        if current is not None and len(found[current]) < OUTPUT_LINES:
            found[current].append(line)
    return {n: _trim(v) for n, v in found.items()}


def outputs_from_junit(path: Path, names: list[str]) -> dict[str, list[str]]:
    wanted, found = set(names), {}
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError):
        return {}
    for case in tree.getroot().iter("testcase"):
        name = case.get("name", "")
        if name in wanted:
            out = case.findtext("system-out") or ""
            found[name] = _trim(out.replace("\r", "").splitlines()[:OUTPUT_LINES])
    return found


def _trim(lines: list[str]) -> list[str]:
    while lines and not lines[-1].strip():
        lines.pop()
    return [ln[:LINE_CHARS] for ln in lines]


def first_errors(text: str) -> list[list[str]]:
    """The first lines that look like configure, compiler or linker errors, with a few lines after."""
    lines = text.splitlines()
    blocks, i = [], 0
    while i < len(lines) and len(blocks) < MAX_ERRORS:
        if BUILD_ERROR.search(lines[i]) and not lines[i].startswith("FAILED: "):
            block = [ln[:LINE_CHARS] for ln in lines[i:i + 1 + ERROR_CONTEXT]]
            blocks.append(_trim(block))
            i += 1 + ERROR_CONTEXT
        else:
            i += 1
    if not blocks:
        tail = [ln[:LINE_CHARS] for ln in lines[-8:]]
        if tail:
            blocks.append(tail)
    return blocks


def failed_targets(text: str) -> list[str]:
    return [ln[len("FAILED: "):][:200] for ln in text.splitlines() if ln.startswith("FAILED: ")][:5]


def summarize_log(path: Path) -> Result:
    result = Result(name=str(path), kind="log", logs={"log": str(path)})
    try:
        text = read_text(path)
    except OSError as e:
        result.ok, result.stage, result.note = False, "read", f"cannot read the log: {e.strerror}"
        return result
    if parse_ctest(text, result):
        result.stage = "test"
        if result.failed_tests:
            result.ok = False
            names = [n for n, _ in result.failed_tests[:MAX_FAILED_OUTPUTS]]
            result.outputs = outputs_from_log(text, names)
        else:
            result.stage = None
        return result
    result.ok = False
    if "Test project " in text:
        result.stage = "test"
        result.note = "ctest did not print its summary (interrupted, or it failed before running the tests)"
        result.errors = [_trim([ln[:LINE_CHARS] for ln in text.splitlines()[-8:]])]
    elif "ninja: build stopped" in text or "FAILED: " in text or "error:" in text:
        result.stage = "build"
        result.errors = first_errors(text)
        targets = failed_targets(text)
        if targets:
            result.note = "failed: " + "; ".join(targets)
    elif "CMake Error" in text or "Configuring incomplete" in text:
        result.stage = "configure"
        result.errors = first_errors(text)
    else:
        result.stage = "unknown"
        result.note = "no ctest summary, build error or CMake error found in the log"
    return result


# --- running presets ----------------------------------------------------------------------------


def host_system() -> str:
    """The value CMake uses for ${hostSystemName}."""
    s = platform.system()
    if s.startswith(("MSYS", "MINGW", "CYGWIN")) or s == "Windows":
        return "Windows"
    return s


def load_presets() -> dict[str, dict]:
    """Visible configure presets that also have a build and a test preset of the same name."""
    found: dict[str, dict] = {}
    data: dict[str, list] = {"configurePresets": [], "buildPresets": [], "testPresets": []}
    for name in ("CMakePresets.json", "CMakeUserPresets.json"):
        path = ROOT / name
        if not path.exists():
            continue
        try:
            loaded = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            die_usage(f"cannot read {name}: {e}")
        for key in data:
            data[key].extend(loaded.get(key, []))
    by_name = {p["name"]: p for p in data["configurePresets"]}
    builds = {p["name"] for p in data["buildPresets"]}
    tests = {p["name"]: p for p in data["testPresets"]}
    for name, preset in by_name.items():
        if preset.get("hidden") or name not in builds or name not in tests:
            continue
        found[name] = {"host": _required_host(preset, by_name), "test_jobs": _test_jobs(tests[name], tests)}
    return found


def _test_jobs(preset: dict, by_name: dict[str, dict]) -> int | None:
    """execution.jobs of a test preset, or of the first preset it inherits that sets it."""
    jobs = preset.get("execution", {}).get("jobs")
    if isinstance(jobs, int):
        return jobs
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        if parent in by_name:
            jobs = _test_jobs(by_name[parent], by_name)
            if jobs is not None:
                return jobs
    return None


def default_test_jobs(preset_jobs: int | None) -> int | None:
    """ctest -j when --test-jobs is not given: the test preset's, but no more than this computer's CPUs (more tests
    at once than CPUs slow down the tests that wait in real time). None: leave it to the preset."""
    cpus = os.cpu_count()
    if preset_jobs is not None and cpus is not None and cpus < preset_jobs:
        return cpus
    return None


def _required_host(preset: dict, by_name: dict[str, dict]) -> str | None:
    cond = preset.get("condition")
    if isinstance(cond, dict) and cond.get("type") == "equals" and cond.get("lhs") == "${hostSystemName}":
        return cond.get("rhs")
    parents = preset.get("inherits", [])
    for parent in [parents] if isinstance(parents, str) else parents:
        if parent in by_name:
            host = _required_host(by_name[parent], by_name)
            if host:
                return host
    return None


def run_step(cmd: list[str], log: Path) -> tuple[int, float]:
    start = time.monotonic()
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write("$ " + " ".join(cmd) + "\n")
        f.flush()
        code = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL).returncode
    return code, time.monotonic() - start


def rel(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def run_preset(preset: str, args: argparse.Namespace, test_jobs: int | None) -> Result:
    result = Result(name=preset, kind="preset")
    base = LOG_DIR / f"{args.tag}-{preset}"
    logs = {stage: Path(f"{base}-{stage}.log") for stage in ("configure", "build", "test")}
    junit = Path(f"{base}.junit.xml")
    for stale in [*logs.values(), junit]:
        stale.unlink(missing_ok=True)

    if not args.no_configure:
        cmd = ["cmake", "--preset", preset]
        if not args.no_werror:
            cmd.append("-DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON")
        code, result.seconds["configure"] = run_step(cmd, logs["configure"])
        result.logs["configure"] = rel(logs["configure"])
        if code != 0:
            result.ok, result.stage = False, "configure"
            result.errors = first_errors(read_text(logs["configure"]))
            return result

    cmd = ["cmake", "--build", "--preset", preset, "-j", str(args.jobs)]
    if args.targets:
        cmd += ["--target", *args.targets]
    code, result.seconds["build"] = run_step(cmd, logs["build"])
    result.logs["build"] = rel(logs["build"])
    if code != 0:
        text = read_text(logs["build"])
        result.ok, result.stage = False, "build"
        result.errors = first_errors(text)
        targets = failed_targets(text)
        if targets:
            result.note = "failed: " + "; ".join(targets)
        return result

    # The test preset already sets the output on failure and the parallelism: override the parallelism on request,
    # or when this computer has fewer CPUs than the preset runs tests at once.
    cmd = ["ctest", "--preset", preset, "--output-junit", str(junit)]
    if test_jobs is not None:
        cmd += ["-j", str(test_jobs)]
    if args.tests:
        cmd += ["-R", args.tests]
    if args.repeat > 1:
        cmd += ["--repeat", f"until-fail:{args.repeat}"]
    if args.timeout is not None:
        cmd += ["--timeout", str(args.timeout)]
    code, result.seconds["test"] = run_step(cmd, logs["test"])
    result.logs["test"] = rel(logs["test"])
    if junit.exists():
        result.logs["junit"] = rel(junit)
    text = read_text(logs["test"])
    if not parse_ctest(text, result):
        result.ok, result.stage = False, "test"
        result.note = f"ctest exited with {code} without its summary"
        result.errors = [_trim([ln[:LINE_CHARS] for ln in text.splitlines()[-8:]])]
        return result
    if code != 0 or result.failed_tests:
        result.ok, result.stage = False, "test"
        names = [n for n, _ in result.failed_tests[:MAX_FAILED_OUTPUTS]]
        outputs = outputs_from_junit(junit, names) if junit.exists() else {}
        if any(not outputs.get(n) for n in names):
            outputs = {**outputs_from_log(text, names), **{n: v for n, v in outputs.items() if v}}
        result.outputs = outputs
        if not result.failed_tests:
            result.note = f"ctest exited with {code}"
    return result


# --- printing -----------------------------------------------------------------------------------


def print_result(r: Result) -> None:
    times = ", ".join(f"{stage} {secs:.0f} s" for stage, secs in r.seconds.items())
    counts = r.counts()
    head = f"{r.name}: " + ("ok" if r.ok else f"FAILED ({r.stage})")
    if counts:
        head += f" - {counts}"
    if times:
        head += f" ({times})"
    print(head)
    if r.skipped_tests:
        print("  skipped: " + ", ".join(r.skipped_tests))
    if r.failed_tests:
        shown = [f"{n} ({s})" for n, s in r.failed_tests[:MAX_FAILED_NAMES]]
        more = len(r.failed_tests) - len(shown)
        print("  failed: " + ", ".join(shown) + (f", and {more} more" if more > 0 else ""))
        for name, _ in r.failed_tests[:MAX_FAILED_OUTPUTS]:
            lines = r.outputs.get(name)
            if lines:
                print(f"  --- {name}")
                for ln in lines:
                    print("    " + ln)
    if r.note:
        print("  " + r.note)
    for block in r.errors:
        print("  ---")
        for ln in block:
            print("    " + ln)
    if not r.ok:
        for kind, path in r.logs.items():
            print(f"  {kind}: {path}")


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/verify.py",
        description="Configure, build and test CMake presets and print a short summary; the full output "
        "goes to build/logs/. Exit code 0 if every preset passed, 1 if one failed, 2 on a usage error.",
        epilog="examples:\n"
        "  scripts/verify.py --linux                       # the presets to pass before a commit\n"
        "  scripts/verify.py linux-gcc-debug --tests '^launch'\n"
        "  scripts/verify.py --linux --repeat 3 --tag nightly\n"
        "  scripts/verify.py --summarize build/logs/verify-linux-gcc-debug-test.log",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("presets", nargs="*", metavar="preset", help="configure/build/test preset names")
    parser.add_argument("--linux", action="store_true", help="add the Linux presets: " + ", ".join(LINUX_PRESETS))
    parser.add_argument("--summarize", nargs="+", metavar="LOG", help="run nothing; summarize existing ctest or build logs")
    parser.add_argument("--tag", default="verify", help="prefix of the log names (default: verify)")
    parser.add_argument("--tests", metavar="REGEX", help="run only the tests that match (ctest -R)")
    parser.add_argument("--targets", nargs="+", metavar="T", help="build only these targets")
    parser.add_argument("--repeat", type=int, default=1, metavar="N", help="ctest --repeat until-fail:N (default: 1)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4, metavar="N", help="build jobs (default: the number of CPUs)")
    parser.add_argument("--test-jobs", type=int, metavar="N", help="ctest -j (default: the test preset's, 8, but no more than the number of CPUs)")
    parser.add_argument("--timeout", type=int, metavar="SEC", help="ctest --timeout (default: none, as the test preset)")
    parser.add_argument("--no-werror", action="store_true",
                        help="do not pass -DVEHICLE_SIL_WARNINGS_AS_ERRORS=ON (the CMake cache keeps its earlier value)")
    parser.add_argument("--no-configure", action="store_true", help="skip the configure step")
    parser.add_argument("--json", action="store_true", help='print the summary as JSON ({"vehicle_sil_verify": 1, ...})')
    args = parser.parse_args()

    if args.summarize:
        if args.presets or args.linux:
            die_usage("--summarize takes log files only; leave out the preset names and --linux")
        results = [summarize_log(Path(p)) for p in args.summarize]
    else:
        presets = list(dict.fromkeys(([*LINUX_PRESETS] if args.linux else []) + args.presets))
        if not presets:
            die_usage("name at least one preset, or use --linux or --summarize (see --help)")
        if args.repeat < 1 or args.jobs < 1 or (args.test_jobs is not None and args.test_jobs < 1):
            die_usage("--repeat, --jobs and --test-jobs must be 1 or more")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", args.tag):
            die_usage(f"--tag {args.tag!r}: use letters, digits, '.', '_' and '-' only (it becomes part of file names)")
        known = load_presets()
        unknown = [p for p in presets if p not in known]
        if unknown:
            die_usage(f"unknown preset {', '.join(unknown)}; CMakePresets.json has: {', '.join(sorted(known))}")
        here = host_system()
        foreign = [p for p in presets if known[p]["host"] not in (None, here)]
        if foreign:
            die_usage(f"{', '.join(foreign)} cannot run on this {here} host (the presets require another host system)")
        for tool in ("cmake", "ctest"):
            if shutil.which(tool) is None:
                die_usage(f"{tool} is not on PATH; install CMake 3.21 or newer")
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        results = []
        try:
            for preset in presets:
                test_jobs = args.test_jobs if args.test_jobs is not None else default_test_jobs(known[preset]["test_jobs"])
                r = run_preset(preset, args, test_jobs)
                results.append(r)
                if not args.json:
                    print_result(r)
                    sys.stdout.flush()
        except KeyboardInterrupt:
            print("verify: interrupted", file=sys.stderr)
            return 130

    failed = [r for r in results if not r.ok]
    if args.json:
        print(json.dumps({"vehicle_sil_verify": 1, "ok": not failed, "results": [r.to_json() for r in results]}, indent=2))
    else:
        if args.summarize:
            for r in results:
                print_result(r)
        if failed:
            print("verify: FAILED " + ", ".join(f"{r.name} ({r.stage})" for r in failed))
        else:
            what = "logs" if args.summarize else "presets"
            print(f"verify: {len(results)} of {len(results)} {what} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(job_summary.run(main))
