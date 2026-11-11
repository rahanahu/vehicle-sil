#!/usr/bin/env python3
"""Apply source mutations one at a time and check that a test fails for each.

For each mutation: replace `old` by `new` in `file`, build the targets, run the tests, and restore the
file from the content read before the change (not from Git: the working tree often has uncommitted
changes). The file is restored on errors, Ctrl+C and SIGTERM too. A mutation is "killed" when a test
fails, "SURVIVED" when every test passes, and "INVALID" when it does not build (it says nothing about
the tests: rewrite it so that it builds). At the end the targets are built again from the restored
sources.

The mutation list is a TOML file:

    build_dir = "build/linux-gcc-debug"     # relative to the repository root
    # optional: jobs (build, default the CPUs), test_jobs (default 8), timeout (seconds per test, 300)

    [groups.unit]
    targets = ["vehicle_sil_launch_tests"]
    tests = '^launch\\.'                      # ctest -R

    [[mutation]]
    name = "limit_same"
    group = "unit"                           # or targets/tests directly in the mutation
    file = "libs/launch/src/run.cpp"         # relative to the repository root
    old = '''    return a + b;'''                 # must occur exactly once in the file
    new = '''    return a;'''

Exit codes: 0 every mutation killed, 1 a mutation survived or was invalid or a step went wrong, 2 usage
error.
Requires Python 3.11+ (standard library only); runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import os
import re
import signal
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LISTED_TEST = re.compile(r"^\s*\d+ - (\S+) \(([^)]*)\)")
SHOWN_FAILURES = 3


class Interrupted(Exception):
    pass


@dataclass
class Mutation:
    name: str
    file: str
    old: str
    new: str
    targets: list[str]
    tests: str


def usage(message: str) -> None:
    print(f"mutate: error: {message}", file=sys.stderr)
    sys.exit(2)


def load(path: Path) -> tuple[dict, list[Mutation]]:
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except OSError as e:
        usage(f"cannot read {path}: {e.strerror}")
    except tomllib.TOMLDecodeError as e:
        usage(f"{path}: {e}")
    if not isinstance(data.get("build_dir"), str):
        usage(f'{path}: set build_dir, e.g. build_dir = "build/linux-gcc-debug"')
    groups = data.get("groups", {})
    mutations, seen = [], set()
    for i, m in enumerate(data.get("mutation", []), 1):
        where = f"{path}: mutation {i} ({m.get('name', 'no name')})"
        for key in ("name", "file", "old", "new"):
            if not isinstance(m.get(key), str):
                usage(f"{where}: '{key}' is missing or not a string")
        if m["name"] in seen:
            usage(f"{where}: the name is used twice")
        seen.add(m["name"])
        group = {}
        if "group" in m:
            if m["group"] not in groups:
                usage(f"{where}: no [groups.{m['group']}]; defined: {', '.join(groups) or 'none'}")
            group = groups[m["group"]]
        targets, tests = m.get("targets", group.get("targets")), m.get("tests", group.get("tests"))
        if not isinstance(targets, list) or not targets or not isinstance(tests, str):
            usage(f"{where}: give 'targets' (a list) and 'tests' (a ctest -R regex), in the mutation or its group")
        if m["old"] == m["new"]:
            usage(f"{where}: 'old' and 'new' are the same")
        mutations.append(Mutation(m["name"], m["file"], m["old"], m["new"], targets, tests))
    if not mutations:
        usage(f"{path}: no [[mutation]] entries")
    return data, mutations


def encoded(m: Mutation, src: bytes) -> tuple[bytes, bytes]:
    """old and new as bytes, with CRLF line ends when the file has them (a Windows checkout)."""
    old, new = m.old, m.new
    if b"\r\n" in src and "\r\n" not in old:
        old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
    return old.encode("utf-8"), new.encode("utf-8")


def check_patterns(mutations: list[Mutation]) -> list[str]:
    problems = []
    for m in mutations:
        path = ROOT / m.file
        try:
            src = path.read_bytes()
        except OSError as e:
            problems.append(f"{m.name}: cannot read {m.file}: {e.strerror}")
            continue
        n = src.count(encoded(m, src)[0])
        if n != 1:
            problems.append(f"{m.name}: 'old' occurs {n} times in {m.file} (it must occur exactly once)")
    return problems


def run(cmd: list[str], log, timeout: float | None = None) -> subprocess.CompletedProcess:
    log.write("$ " + " ".join(cmd) + "\n")
    log.flush()
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          stdin=subprocess.DEVNULL, timeout=timeout)
    log.write(proc.stdout)
    log.write(proc.stderr)
    log.write(f"[exit {proc.returncode}]\n")
    log.flush()
    return proc


def first_error(text: str) -> str:
    for line in text.splitlines():
        if re.search(r"(^|[\s:])(fatal )?error:|undefined reference", line):
            return line.strip()[:200]
    return ""


def on_signal(signum, frame):
    raise Interrupted()


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/mutate.py",
        description=(__doc__ or "").split("\n\n")[0] + " See the top of this file for the TOML format.",
        epilog="examples:\n"
        "  scripts/mutate.py mutations.toml --check      # only check that every 'old' occurs once\n"
        "  scripts/mutate.py mutations.toml              # run every mutation\n"
        "  scripts/mutate.py mutations.toml m1 m2        # run these mutations\n"
        "exit codes: 0 every mutation killed, 1 a mutation survived or a step failed, 2 usage error",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("list_file", metavar="mutations.toml", help="the mutation list (any location)")
    parser.add_argument("names", nargs="*", metavar="name", help="run only these mutations")
    parser.add_argument("--check", action="store_true", help="only check that each 'old' occurs exactly once")
    parser.add_argument("--list", action="store_true", help="list the mutations and stop")
    parser.add_argument("--jobs", type=int, metavar="N", help="build jobs (default: jobs in the file, else the CPUs)")
    parser.add_argument("--log", metavar="FILE", help="build and test output (default: build/logs/mutate.log)")
    args = parser.parse_args()

    config, mutations = load(Path(args.list_file))
    unknown = [n for n in args.names if n not in {m.name for m in mutations}]
    if unknown:
        usage(f"unknown mutation {', '.join(unknown)}; see --list")
    if args.names:
        mutations = [m for m in mutations if m.name in args.names]
    if args.list:
        for m in mutations:
            print(f"{m.name}  {m.file}  tests {m.tests}")
        return 0

    problems = check_patterns(mutations)
    for p in problems:
        print(p)
    if problems:
        print(f"mutate: {len(problems)} of {len(mutations)} patterns do not match; nothing was changed")
        return 1
    if args.check:
        print(f"mutate: {len(mutations)} patterns ok")
        return 0

    build_dir = config["build_dir"]
    jobs = str(args.jobs or config.get("jobs") or os.cpu_count() or 4)
    test_jobs = str(config.get("test_jobs", 8))
    timeout = str(config.get("timeout", 300))
    log_path = Path(args.log) if args.log else ROOT / "build" / "logs" / "mutate.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGTERM, on_signal)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, on_signal)

    killed = invalid = survived = errors = 0
    touched: list[str] = []
    interrupted = False
    with open(log_path, "w", encoding="utf-8") as log:
        for m in mutations:
            path = ROOT / m.file
            src = path.read_bytes()
            old, new = encoded(m, src)
            touched += [t for t in m.targets if t not in touched]
            log.write(f"\n===== {m.name}: {m.file}\n")
            start = time.monotonic()
            try:
                path.write_bytes(src.replace(old, new, 1))
                build = run(["cmake", "--build", build_dir, "-j", jobs, "--target", *m.targets], log)
                if build.returncode != 0:
                    invalid += 1
                    err = first_error(build.stdout + build.stderr)
                    print(f"{m.name}: INVALID (it does not build)" + (f" - {err}" if err else ""), flush=True)
                    continue
                test = run(["ctest", "--test-dir", build_dir, "-R", m.tests, "--timeout", timeout, "-j", test_jobs], log)
                failed = []
                for line in test.stdout.splitlines():
                    hit = LISTED_TEST.match(line)
                    if hit and hit.group(2) != "Skipped":
                        failed.append(hit.group(1))
                secs = f"{time.monotonic() - start:.0f} s"
                if failed:
                    killed += 1
                    more = f", and {len(failed) - SHOWN_FAILURES} more" if len(failed) > SHOWN_FAILURES else ""
                    print(f"{m.name}: killed ({secs}) - {', '.join(failed[:SHOWN_FAILURES])}{more}", flush=True)
                elif test.returncode != 0:
                    errors += 1
                    last = (test.stdout + test.stderr).strip().splitlines()[-1:] or [""]
                    print(f"{m.name}: ERROR - ctest exited with {test.returncode}: {last[0][:200]}", flush=True)
                else:
                    survived += 1
                    print(f"{m.name}: SURVIVED ({secs}) - every test matching {m.tests} passed", flush=True)
            except (KeyboardInterrupt, Interrupted):
                interrupted = True
                break
            finally:
                # Writing the original back also makes the file newer than the mutated build output.
                path.write_bytes(src)
        if interrupted:
            print("mutate: interrupted; the source is restored. Rebuild before testing: "
                  f"cmake --build {build_dir} --target {' '.join(touched)}", flush=True)
            return 130
        rebuild = run(["cmake", "--build", build_dir, "-j", jobs, "--target", *touched], log)

    total = f"{killed} killed, {survived} survived, {invalid} invalid"
    if errors:
        total += f", {errors} errors"
    print(f"mutate: {total}")
    if rebuild.returncode != 0:
        print(f"mutate: rebuilding {' '.join(touched)} from the restored sources failed; see {log_path}")
        return 1
    if survived or invalid or errors:
        print(f"mutate: log {log_path}")
    return 1 if survived or invalid or errors else 0


if __name__ == "__main__":
    sys.exit(main())
