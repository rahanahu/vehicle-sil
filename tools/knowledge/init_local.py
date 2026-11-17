#!/usr/bin/env python3
"""Set up the local knowledge directory (.local/) of a clone, without overwriting anything.

  python3 tools/knowledge/init_local.py            show what would be created (nothing is written)
  python3 tools/knowledge/init_local.py --apply    create it

It makes the directories of .local/, excludes /.local/ and /CLAUDE.local.md in .git/info/exclude,
and copies the templates in tools/knowledge/templates/ to the five shared files and to
CLAUDE.local.md, but only for a file that does not exist. A file that exists is kept as it is, so
the command is safe in a clone that already has a .local/. It prints what the user still has to do
by hand (the placeholders in the copies, the Copilot and Claude settings).

In a linked working tree it works on the main checkout's .local/.

Exit codes: 0 done (or shown), 2 usage error, not a git repository, or a symbolic link in the way.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DIRS = ("decisions", "plans", "plans/streams", "plans/archive", "reviews", "research", "experiments", "changelog",
        "tools", "vendor")
FILES = {  # template -> path relative to .local, or "../" for the repository root
    "CONSTRAINTS.md": "CONSTRAINTS.md",
    "STATE.md": "plans/STATE.md",
    "RULES.md": "plans/RULES.md",
    "ACTIVE.md": "decisions/ACTIVE.md",
    "pending.md": "changelog/pending.md",
    "CLAUDE.local.md": "../CLAUDE.local.md",
}
EXCLUDES = ("/.local/", "/CLAUDE.local.md")


def git_dir(repo: Path) -> Path | None:
    """The git directory that holds info/exclude. For a linked working tree it is the common one."""
    git = repo / ".git"
    if git.is_dir():
        return git
    if git.is_file():
        try:
            gitdir = (repo / git.read_text().split("gitdir:", 1)[1].strip()).resolve()  # may be relative
            return (gitdir / (gitdir / "commondir").read_text().strip()).resolve()
        except (OSError, IndexError):
            return None
    return None


def run(repo: Path, apply: bool) -> int:
    common = git_dir(repo)
    if common is None:
        print(f"init_local: FAIL {repo} is not a git repository.", file=sys.stderr)
        return 2
    if (repo / ".git").is_file():  # a linked working tree has no .local of its own: use the main checkout
        repo = common.parent
    local = repo / ".local"
    if local.is_symlink() or (repo / "CLAUDE.local.md").is_symlink():
        print("init_local: FAIL .local or CLAUDE.local.md is a symbolic link; nothing was written.", file=sys.stderr)
        return 2
    verb = "create" if apply else "would create"
    for rel in DIRS:
        if not (local / rel).is_dir():
            print(f"{verb} {local.name}/{rel}/")
            if apply:
                (local / rel).mkdir(parents=True, exist_ok=True)
    exclude = common / "info" / "exclude"
    have = exclude.read_text().splitlines() if exclude.is_file() else []
    for line in EXCLUDES:
        if line not in have:
            print(f"{'add' if apply else 'would add'} {line} to .git/info/exclude")
            if apply:
                exclude.parent.mkdir(parents=True, exist_ok=True)
                with exclude.open("a") as f:
                    f.write(("" if not have or exclude.read_text().endswith("\n") else "\n") + line + "\n")
                have.append(line)
    for template, rel in FILES.items():
        target = repo / "CLAUDE.local.md" if rel.startswith("../") else local / rel
        shown = rel.replace("../", "")
        if target.exists() or target.is_symlink():
            print(f"keep {shown} (exists)")
            continue
        print(f"{verb} {shown}")
        if apply:
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("x", encoding="utf-8") as out:  # "x": never replaces a file that appeared meanwhile
                out.write((HERE / "templates" / template).read_text(encoding="utf-8"))
    print("next: replace the <placeholders> in the new files; see tools/knowledge/README.md for the Claude and "
          "Copilot settings." if apply else "nothing was written; run again with --apply.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="init_local.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("--apply", action="store_true", help="create the files (default: only show)")
    parser.add_argument("--repo", default=str(HERE.parents[1]), help="the repository (default: the one of this script)")
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 0 if e.code == 0 else 2
    return run(Path(args.repo).resolve(), args.apply)


if __name__ == "__main__":
    sys.exit(main())
