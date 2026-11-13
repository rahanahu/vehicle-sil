#!/usr/bin/env python3
"""Check the links of the Markdown files: the file, the heading, and a section named after the link.

  scripts/check-docs.py              every tracked *.md
  scripts/check-docs.py FILE...      these files only

Checked in each relative link: the file exists, a #fragment is a heading of it (as GitHub makes the anchor),
and a section named in quotes right after the link (...) の「name」 occurs in the file linked. Links with a
scheme (https:, mailto:) and text inside code are not looked at. Each problem is one line,
"<file>:<line>: <text>". Exit codes: 0 no problem, 1 a problem, 2 usage error. Requires Python 3.11+ (standard
library only); runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def strip_code(text):
    out, fence = [], False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fence = not fence
            out.append("")
            continue
        out.append("" if fence else re.sub(r"`[^`]*`", lambda m: "\0" * len(m.group(0)), line))
    return "\n".join(out)


def slug(heading):
    s = heading.strip().lower()
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)
    kept = []
    for ch in s:
        cat = unicodedata.category(ch)
        if ch in " -_" or cat[0] in "LMN" or cat == "Pc":
            kept.append(ch)
    return "".join(kept).replace(" ", "-")


anchor_cache = {}


def anchors(path):
    if path not in anchor_cache:
        result, seen, fence = set(), {}, False
        for line in path.read_text(encoding="utf-8").split("\n"):
            if line.lstrip().startswith("```"):
                fence = not fence
                continue
            m = None if fence else re.match(r"^(#{1,6})\s+(.*)$", line)
            if m:
                base = slug(m.group(2))
                n = seen.get(base, 0)
                result.add(base if n == 0 else f"{base}-{n}")
                seen[base] = n + 1
        anchor_cache[path] = result
    return anchor_cache[path]


def tracked() -> list[Path]:
    proc = subprocess.run(["git", "-c", "core.quotePath=false", "ls-files", "*.md"], cwd=ROOT, capture_output=True,
                          text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        print("check-docs: git ls-files failed: name the files to check", file=sys.stderr)
        sys.exit(2)
    return [ROOT / line for line in proc.stdout.splitlines() if line and (ROOT / line).is_file()]


def check(files: list[Path]) -> tuple[list[str], int, int]:
    problems, links, quotes = [], 0, 0
    for f in files:
        raw = f.read_text(encoding="utf-8")
        text = strip_code(raw)
        raw_lines = raw.split("\n")
        for lineno, line in enumerate(text.split("\n"), 1):
            for m in re.finditer(r"\[[^\]]*\]\(([^)\s]+)\)((?: の「[^」]+」)*)", line):
                target = m.group(1)
                quoted = raw_lines[lineno - 1][m.start(2):m.end(2)]
                if re.match(r"^[a-z]+:", target):
                    continue
                links += 1
                path_part, _, frag = target.partition("#")
                dest = f if not path_part else (f.parent / path_part).resolve()
                where = f"{shown(f)}:{lineno}"
                if not dest.exists():
                    problems.append(f"{where}: missing file {target}")
                    continue
                if frag and dest.suffix == ".md" and frag not in anchors(dest):
                    problems.append(f"{where}: missing anchor #{frag} in {shown(dest)}")
                if quoted and dest.suffix == ".md":
                    body = dest.read_text(encoding="utf-8").replace("`", "")
                    for name in re.findall(r"「([^」]+)」", quoted):
                        quotes += 1
                        if name.replace("`", "") not in body:
                            problems.append(f"{where}: 「{name}」 not found in {shown(dest)}")
    return problems, links, quotes


def shown(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", type=Path, help="Markdown files (default: every tracked *.md)")
    args = parser.parse_args(argv)
    files = [f.resolve() for f in args.files] if args.files else tracked()
    for f in files:
        if not f.is_file():
            print(f"check-docs: no such file: {f}", file=sys.stderr)
            return 2
    problems, links, quotes = check(files)
    for problem in problems:
        print(problem)
    print(f"check-docs: {'FAILED' if problems else 'ok'} - {len(files)} files, {links} relative links, "
          f"{quotes} named sections, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
