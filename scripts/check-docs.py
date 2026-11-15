#!/usr/bin/env python3
"""Check the Markdown files: their links, and that each English page and its Japanese page agree.

  scripts/check-docs.py                      every tracked *.md
  scripts/check-docs.py FILE...              these files only (and the other page of each pair)
  scripts/check-docs.py --require-english    a Japanese page without its English page is a problem too

Links, in every file: in each relative link the file exists, a #fragment is a heading of it (as GitHub makes the
anchor), and a section named in quotes right after the link (...) の「name」 occurs in the file linked. Links with a
scheme (https:, mailto:) and text inside code are not looked at.

Pairs: the English page docs/<path> has its Japanese page docs/ja/<path>, and <dir>/README.md has
<dir>/README.ja.md (the files of ENGLISH_ONLY have none). Of each pair: both pages exist; the headings have the same
levels in the same order; the fenced code blocks have the same language tags in the same order, a block with only
ASCII in the Japanese page is the same, byte for byte, in the English page, and a block with Japanese in it has the
same number of lines; a page links the pages of its own language, and both pages link the same files. A Japanese
page whose English page is not written yet is counted, not a problem, unless --require-english; a link to such an
English page is then not followed either.

Each problem is one line, "<file>:<line>: <text>". Exit codes: 0 no problem, 1 a problem, 2 usage error. Requires
Python 3.11+ (standard library only); runs on Linux and in MSYS2 on Windows.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

import job_summary

ROOT = Path(__file__).resolve().parent.parent

# Pages with no Japanese page, on purpose: the instructions for agents, the change log, the license notices, the
# notes on the upstream patches, and the notes on test data.
ENGLISH_ONLY = {"AGENTS.md", "CHANGELOG.md", "CLAUDE.md", "THIRD_PARTY_NOTICES.md", "patches/README.md",
                "tests/control_server/testdata/README.md"}


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


# --- pairs ----------------------------------------------------------------------------------------------------------

def rel(path: Path) -> str | None:
    """The path relative to ROOT, with "/", or None outside it."""
    try:
        return path.resolve().relative_to(ROOT.resolve()).as_posix()
    except ValueError:
        return None


def japanese_of(english: str) -> str | None:
    """The Japanese page of an English page (paths relative to ROOT), or None if it has none."""
    if english in ENGLISH_ONLY or not english.endswith(".md"):
        return None
    if english.startswith("docs/") and not english.startswith("docs/ja/"):
        return "docs/ja/" + english[len("docs/"):]
    if english == "README.md" or english.endswith("/README.md"):
        return english[:-len("README.md")] + "README.ja.md"
    return None


def english_of(path: str) -> str | None:
    """The English counterpart of a file of the Japanese pages (docs/ja/..., README.ja.md), or None."""
    if path.startswith("docs/ja/"):
        return "docs/" + path[len("docs/ja/"):]
    if path == "README.ja.md" or path.endswith("/README.ja.md"):
        return path[:-len("README.ja.md")] + "README.md"
    return None


def pending_english(dest: Path) -> bool:
    """dest is an English page that is not written yet, and its Japanese page is."""
    r = rel(dest)
    ja = japanese_of(r) if r else None
    return ja is not None and not dest.exists() and (ROOT / ja).is_file()


def headings(text: str) -> list[tuple[int, int]]:
    """(level, line) of each heading outside code."""
    out, fence = [], False
    for lineno, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("```"):
            fence = not fence
            continue
        m = None if fence else re.match(r"^(#{1,6})\s", line)
        if m:
            out.append((len(m.group(1)), lineno))
    return out


def code_blocks(text: str) -> list[tuple[str, list[str], int]]:
    """(language tag, lines, line of the opening fence) of each fenced code block."""
    out, current = [], None
    for lineno, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("```"):
            if current is None:
                current = (line.lstrip()[3:].strip(), [], lineno)
            else:
                out.append(current)
                current = None
        elif current is not None:
            current[1].append(line)
    return out


def compare(en: str, ja: str, en_text: str, ja_text: str, en_links: dict, ja_links: dict) -> list[str]:
    """What differs between an English page and its Japanese page."""
    problems = []
    fix = "give both pages the same headings"
    eh, jh = headings(en_text), headings(ja_text)
    for i, ((el, eline), (jl, jline)) in enumerate(zip(eh, jh), 1):
        if el != jl:
            problems.append(f"{en}:{eline}: heading {i} is of level {el}, heading {i} of {ja} (line {jline}) is of "
                            f"level {jl}: {fix}")
            break
    else:
        if len(eh) != len(jh):
            problems.append(f"{en}: {len(eh)} headings, {ja} has {len(jh)}: {fix}")
    eb, jb = code_blocks(en_text), code_blocks(ja_text)
    if len(eb) != len(jb):
        problems.append(f"{en}: {len(eb)} code blocks, {ja} has {len(jb)}: give both pages the same code blocks")
    for i, ((etag, elines, eline), (jtag, jlines, jline)) in enumerate(zip(eb, jb), 1):
        where = f"{en}:{eline}: code block {i}"
        other = f"the block of {ja}:{jline}"
        if etag != jtag:
            problems.append(f"{where} is tagged '{etag}', {other} '{jtag}': tag both the same")
            break
        if all(ord(c) < 128 for line in jlines for c in line):
            if elines != jlines:
                problems.append(f"{where} differs from {other}, which has no Japanese in it: copy it byte for byte")
        elif len(elines) != len(jlines):
            problems.append(f"{where} has {len(elines)} lines, {other} has {len(jlines)}: keep the lines, translate "
                            f"only the Japanese in them")
    for page, mine, theirs, other_page in ((ja, ja_links, en_links, en), (en, en_links, ja_links, ja)):
        for dest, line in sorted(mine.items(), key=lambda item: item[1]):
            if dest not in theirs:
                problems.append(f"{page}:{line}: links {dest}, which {other_page} does not: link the same files "
                                f"from both pages")
    return problems


# --- the check ------------------------------------------------------------------------------------------------------

def check(files: list[Path], require_english: bool = False) -> tuple[list[str], int, int, list[str], int]:
    """(problems, links, named sections, Japanese pages without their English page, links to those)."""
    problems, links, quotes, pending_links = [], 0, 0, 0
    page_links: dict[str, dict[str, int]] = {}  # page -> {the file it links, as its English path: first line}

    def scan(f: Path) -> None:
        nonlocal links, quotes, pending_links
        raw = f.read_text(encoding="utf-8")
        text = strip_code(raw)
        raw_lines = raw.split("\n")
        me = rel(f)
        japanese_page = me is not None and english_of(me) is not None
        found = page_links.setdefault(me or str(f), {})
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
                d = rel(dest)
                if d is not None:
                    found.setdefault(english_of(d) or d, lineno)
                    if dest.suffix == ".md" and japanese_page and japanese_of(d):
                        problems.append(f"{where}: links the English page {d}: link its Japanese page {japanese_of(d)}")
                    elif dest.suffix == ".md" and not japanese_page and english_of(d):
                        problems.append(f"{where}: links the Japanese page {d}: link its English page {english_of(d)}")
                if not dest.exists():
                    if not require_english and pending_english(dest):
                        pending_links += 1
                    else:
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

    for f in files:
        scan(f)

    pairs: list[tuple[str, str]] = []
    for f in files:
        me = rel(f)
        if me is None:
            continue
        pair = (english_of(me), me) if english_of(me) else (me, japanese_of(me))
        if pair[1] is not None and pair not in pairs:
            pairs.append(pair)
    pending = []
    for en, ja in pairs:
        en_path, ja_path = ROOT / en, ROOT / ja
        if not ja_path.is_file():
            problems.append(f"{en}: no Japanese page {ja}: write it (the same headings and code blocks), or add {en} "
                            f"to ENGLISH_ONLY in scripts/check-docs.py if it has none on purpose")
            continue
        if not en_path.is_file():
            if require_english:
                problems.append(f"{ja}: no English page {en}: write it (the same headings and code blocks)")
            else:
                pending.append(ja)
            continue
        for page in (en, ja):
            if page not in page_links:
                scan(ROOT / page)
        problems += compare(en, ja, en_path.read_text(encoding="utf-8"), ja_path.read_text(encoding="utf-8"),
                            page_links[en], page_links[ja])
    return problems, links, quotes, pending, pending_links


def shown(path: Path) -> str:
    r = rel(path)
    return r if r is not None else str(path)


def main(argv: list[str]) -> int:
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", type=Path, help="Markdown files (default: every tracked *.md)")
    parser.add_argument("--require-english", action="store_true",
                        help="a Japanese page without its English page, and a link to that missing English page, "
                             "is a problem (without it, they are counted)")
    parser.add_argument("--root", type=Path, default=ROOT,
                        help="the directory the pairs' paths are relative to (default: this repository)")
    args = parser.parse_args(argv)
    ROOT = args.root.resolve()
    if not ROOT.is_dir():
        print(f"check-docs: no such directory: {args.root}", file=sys.stderr)
        return 2
    files = [f.resolve() for f in args.files] if args.files else tracked()
    for f in files:
        if not f.is_file():
            print(f"check-docs: no such file: {f}", file=sys.stderr)
            return 2
    problems, links, quotes, pending, pending_links = check(files, args.require_english)
    for problem in problems:
        print(problem)
    if pending:
        more = f" and {len(pending) - 1} more" if len(pending) > 1 else ""
        print(f"check-docs: {len(pending)} Japanese pages have no English page yet ({pending[0]}{more}; "
              f"{pending_links} links to them not followed): --require-english makes each a problem")
    print(f"check-docs: {'FAILED' if problems else 'ok'} - {len(files)} files, {links} relative links, "
          f"{quotes} named sections, {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(job_summary.run(lambda: main(sys.argv[1:])))
