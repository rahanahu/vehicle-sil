#!/usr/bin/env python3
"""Check commits against the rules in AGENTS.md: the message format and the files that must not be committed.

  scripts/check-commits.py                 the staged files (before a commit)
  scripts/check-commits.py --message FILE  one commit message (usable as a commit-msg hook)
  scripts/check-commits.py --range A..B    every commit in a range (for CI)

Each problem is one line, "<commit or staged>: <code>: <text>". Exit codes: 0 no problem, 1 a problem,
2 usage error. Requires Python 3.11+ (standard library only); runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TYPES = ["feat", "fix", "refactor", "test", "docs", "build", "ci", "chore", "perf", "style", "revert"]
TITLE = re.compile(r"^(?P<type>[A-Za-z]+)(?:\((?P<scope>[^()]*)\))?(?P<bang>!)?:(?: (?P<text>.*))?$")
UNCHECKED_TITLES = ("Merge ", 'Revert "', "fixup! ", "squash! ", "amend! ")
SCISSORS = "# ------------------------ >8 ------------------------"


def git(*args: str) -> str:
    proc = subprocess.run(["git", "-c", "core.quotePath=false", *args], cwd=ROOT, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        print(f"check-commits: error: git {' '.join(args)}: {proc.stderr.strip()}", file=sys.stderr)
        sys.exit(2)
    return proc.stdout


def path_problems(path: str, fix: str) -> list[tuple[str, str]]:
    if path == ".local" or path.startswith(".local/"):
        return [("local-file", f"{path} is local-only (.local/ is never committed); {fix}")]
    if path == "CLAUDE.local.md":
        return [("local-file", f"{path} holds private instructions and is never committed; {fix}")]
    if path.startswith("build/"):
        return [("build-file", f"{path} is build output; {fix}")]
    return []


def message_problems(message: str) -> list[tuple[str, str]]:
    lines = message.splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    if not lines:
        return [("empty-message", "the commit message is empty")]
    title = lines[0]
    if title.startswith(UNCHECKED_TITLES):
        return []
    problems = []
    m = TITLE.match(title)
    if not m:
        problems.append(("title-format", f"title {title!r} is not 'type(scope): description' "
                         f"(Conventional Commits; e.g. 'fix(j1939): keep the frame order within a step')"))
    else:
        if m.group("type") not in TYPES:
            problems.append(("title-type", f"type {m.group('type')!r} is not one of: {', '.join(TYPES)}"))
        scope = m.group("scope")
        if scope is not None and not re.fullmatch(r"[a-z0-9][a-z0-9_./-]*(,[a-z0-9][a-z0-9_./-]*)*", scope):
            problems.append(("title-scope", f"scope {scope!r}: use lower-case names, separated by ',' "
                             "(e.g. 'core', 'ecu,simulation')"))
        if not (m.group("text") or "").strip():
            problems.append(("title-description", "the title has no description after 'type(scope): '"))
    if len(lines) > 1 and lines[1].strip():
        problems.append(("second-line", "line 2 must be blank: separate the title from the body with an empty line"))
    return problems


def clean_message(text: str) -> str:
    """The message as git stores it: without '#' comment lines and the diff below the scissors line."""
    if SCISSORS in text:
        text = text[:text.index(SCISSORS)]
    return "\n".join(ln.rstrip() for ln in text.splitlines() if not ln.startswith("#"))


def check_staged() -> tuple[list[str], str]:
    out = []
    files = [f for f in git("diff", "--cached", "--name-only", "--diff-filter=d", "-z").split("\0") if f]
    for path in files:
        for code, text in path_problems(path, f"unstage it: git reset -- {path}"):
            out.append(f"staged: {code}: {text}")
    return out, f"staged, {len(files)} file{'s' if len(files) != 1 else ''}"


def check_message(file: str) -> tuple[list[str], str]:
    try:
        text = Path(file).read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"check-commits: error: cannot read {file}: {e.strerror}", file=sys.stderr)
        sys.exit(2)
    return [f"message: {code}: {text}" for code, text in message_problems(clean_message(text))], "message"


def check_range(spec: str) -> tuple[list[str], str]:
    count = int(git("rev-list", "--count", spec, "--").strip() or 0)
    if not count:
        return [], "0 commits"
    out = []
    # One git call for the messages and one for the files, not one per commit.
    for record in git("log", "--format=%H%x00%B%x01", spec, "--").split("\x01"):
        if "\0" not in record:
            continue
        sha, body = record.strip("\n").split("\0", 1)
        for code, text in message_problems(body):
            out.append(f"{sha[:10]}: {code}: {text}")
    sha = None
    for line in git("log", "--format=%x00%H", "--name-only", "--diff-filter=d", spec, "--").splitlines():
        if line.startswith("\0"):
            sha = line[1:]
        elif line and sha:
            for code, text in path_problems(line, "remove it from the commit"):
                out.append(f"{sha[:10]}: {code}: {text}")
    return out, f"{count} commit{'s' if count != 1 else ''}"


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    parser = argparse.ArgumentParser(
        prog="scripts/check-commits.py",
        description="Check the commit message format (Conventional Commits, a blank second line) and that "
        "no local-only or build files are committed (.local/, CLAUDE.local.md, build/), as AGENTS.md "
        "describes. Without options, checks the staged files.",
        epilog="examples:\n"
        "  scripts/check-commits.py                      # before git commit\n"
        "  scripts/check-commits.py --message .git/COMMIT_EDITMSG\n"
        "  scripts/check-commits.py --range origin/main..HEAD\n"
        "  scripts/check-commits.py --range HEAD         # the whole history\n"
        "exit codes: 0 no problem, 1 a problem found, 2 usage error",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--message", metavar="FILE", help="check the commit message in FILE ('#' lines are ignored)")
    group.add_argument("--range", metavar="RANGE", help="check every commit in RANGE (anything git rev-list takes)")
    args = parser.parse_args()

    if args.message:
        problems, what = check_message(args.message)
    elif args.range:
        problems, what = check_range(args.range)
    else:
        problems, what = check_staged()
    for line in problems:
        print(line)
    if problems:
        print(f"check-commits: {len(problems)} problem{'s' if len(problems) > 1 else ''} ({what})")
        return 1
    print(f"check-commits: ok ({what})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
