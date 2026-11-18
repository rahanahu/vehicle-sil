#!/usr/bin/env python3
"""Replace one piece of text in one of the shared files of .local, safely against other writers.

  shared_edit.py FILE (--old TEXT | --old-file PATH) (--new TEXT | --new-file PATH) [--timeout SECONDS]

FILE is one of the six shared files (several coordinators write them), written relative to
.local, relative to the repository root (.local/...) or as an absolute path:
  CONSTRAINTS.md  plans/STATE.md  plans/RULES.md  plans/FLAKY.md  decisions/ACTIVE.md
  changelog/pending.md
The file must already exist. The tool changes nothing else and never creates the file. A stream
file (plans/streams/) is not a shared file: its coordinator edits it directly, and anyone else
tells the coordinator.

What it does, in this order: take an exclusive lock on .local/.shared-edit.lock (one lock for all
shared files; the lock file is never deleted, and the OS drops the lock when the process ends),
open FILE again by its path and read it, check that --old is in the file exactly once, write the
result to a temporary file in the same directory (.<name>.<pid>.<random>.tmp, so that no *.md
search finds it), and rename it over FILE. A writer that is killed leaves either the old file or
the new one, never a half-written one; a left-over *.tmp file is harmless and may be deleted by
hand while no tool is running.

Outcomes (summary on stdout for success, on stderr for failure):
  exit 0  changed.
  exit 1  not changed: no-match (--old is not there: read the file again and rebuild --old from
          its current text), ambiguous (--old is there more than once: add the neighbouring lines),
          lock-timeout (another writer holds the lock: run it again; tell the user if it fails
          twice; never fall back to writing the file directly), io-error.
  exit 2  wrong usage (not one of the six files, a symbolic link, empty --old, a --timeout that is
          negative, NaN or infinite, an --old-file/--new-file inside vendor/ (it is
          refused before it is opened), ...).

--new '' deletes the text. To add a line, take the neighbouring line as --old and put it back in
--new together with the new line. Compare and write are done on bytes (no newline or encoding
conversion); the permissions of FILE are kept.

Recovery: undo one change by running the inverse edit (--old = the text you wrote, --new = the
text that was there). Do not restore a whole file from a backup while other writers may be
running: that discards their changes. Restore a whole file only when every other writer is stopped.
"""

from __future__ import annotations

import argparse
import errno
import math
import os
import sys
import time
from pathlib import Path

try:
    import fcntl
except ImportError:  # not POSIX
    fcntl = None

SHARED_FILES = (
    "CONSTRAINTS.md",
    "plans/STATE.md",
    "plans/RULES.md",
    "plans/FLAKY.md",
    "decisions/ACTIVE.md",
    "changelog/pending.md",
)
LOCK_NAME = ".shared-edit.lock"
DEFAULT_TIMEOUT = 10.0
POLL_SECONDS = 0.02

FORBIDDEN_DIRS = ("vendor",)  # relative to .local; third-party downloads that an agent must not open, even as --old-file


def _local_dir(script: Path = Path(__file__)) -> Path:
    """The .local directory of the repository that holds this script. A linked worktree has no .local
    of its own, so a .git file ("gitdir: ...") is followed to the common git directory."""
    repo = script.resolve().parents[2]
    git = repo / ".git"
    if git.is_file():
        try:
            gitdir = (repo / git.read_text().split("gitdir:", 1)[1].strip()).resolve()  # may be relative
            common = (gitdir / "commondir").read_text().strip()
            repo = (gitdir / common).resolve().parent
        except (OSError, IndexError):
            pass
    return repo / ".local"


DEFAULT_ROOT = _local_dir()


class EditError(Exception):
    def __init__(self, code: int, kind: str, message: str):
        super().__init__(message)
        self.code = code
        self.kind = kind
        self.message = message


def resolve_target(root: Path, name: str) -> str:
    """Return the name, relative to `root`, of the shared file that `name` points to."""
    path = Path(name)
    candidates = [path] if path.is_absolute() else [root / path, root.parent / path]
    for candidate in candidates:
        rel = os.path.relpath(os.path.abspath(candidate), os.path.abspath(root))
        if rel in SHARED_FILES:
            return rel
    hint = " A stream file has one owner: its coordinator edits it directly; others tell the coordinator." \
        if "streams" in Path(name).parts else ""
    raise EditError(2, "usage", f"{name} is not a shared file. The tool writes only: {', '.join(SHARED_FILES)} "
                    f"(relative to .local).{hint}")


def _acquire(lock_path: Path, timeout: float) -> int:
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    deadline = time.monotonic() + timeout
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fd
        except OSError as e:
            if e.errno not in (errno.EWOULDBLOCK, errno.EAGAIN, errno.EACCES):
                os.close(fd)
                raise
        if time.monotonic() >= deadline:
            os.close(fd)
            raise EditError(1, "lock-timeout", f"another writer held {LOCK_NAME} for {timeout:g} s. Run the same command "
                            "again; if it fails twice, tell the user. Do not write the file directly.")
        time.sleep(POLL_SECONDS)


def edit_file(root: Path, name: str, old: bytes, new: bytes, timeout: float = DEFAULT_TIMEOUT) -> tuple[str, int, int]:
    if fcntl is None:
        raise EditError(2, "usage", "this tool needs a POSIX system (fcntl).")
    if not math.isfinite(timeout) or timeout < 0:
        raise EditError(2, "usage", f"--timeout must be a finite number of seconds, 0 or more (got {timeout}).")
    if not old:
        raise EditError(2, "usage", "--old is empty. Give the text to replace.")
    if old == new:
        raise EditError(2, "usage", "--old and --new are the same; there is nothing to change.")
    rel = resolve_target(root, name)
    target = root / rel
    if not root.is_dir():
        raise EditError(2, "usage", f"{root} does not exist. Run tools/knowledge/init_local.py --apply first.")
    try:
        lock_fd = _acquire(root / LOCK_NAME, timeout)
    except OSError as e:
        raise EditError(1, "io-error", f"cannot take the lock {LOCK_NAME}: {e.strerror}. Nothing was changed.") from e
    tmp = None
    try:
        try:
            st = os.lstat(target)
        except FileNotFoundError:
            raise EditError(2, "usage", f"{rel} does not exist. The tool does not create shared files.") from None
        if not os.path.isfile(target) or os.path.islink(target) \
                or os.path.realpath(target) != os.path.join(os.path.realpath(root), rel):
            raise EditError(2, "usage", f"{rel} is not a plain file inside .local (a symbolic link or a directory).")
        with os.fdopen(os.open(target, os.O_RDONLY | os.O_NOFOLLOW), "rb") as f:
            data = f.read()
        first = data.find(old)
        if first < 0:
            raise EditError(1, "no-match", f"the text to replace is not in {rel}. Someone may have changed it: read the "
                            "file again and rebuild --old from the current text.")
        if data.find(old, first + 1) >= 0:
            raise EditError(1, "ambiguous", f"the text to replace is in {rel} more than once. Add the neighbouring lines "
                            "to --old so that it matches once.")
        result = data[:first] + new + data[first + len(old):]
        tmp_path = target.with_name(f".{target.name}.{os.getpid()}.{os.urandom(4).hex()}.tmp")
        tmp_fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        tmp = tmp_path  # set only once this call has created the file, so that a failure never deletes someone else's file
        with os.fdopen(tmp_fd, "wb") as f:
            os.fchmod(f.fileno(), st.st_mode & 0o7777)
            f.write(result)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, target)
        tmp = None
        return rel, len(data), len(result)
    except EditError:
        raise
    except OSError as e:
        raise EditError(1, "io-error", f"{e.strerror or e} ({rel}). Nothing was changed. Tell the user.") from e
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        os.close(lock_fd)


def _refuse_forbidden(path: str, root: Path, what: str) -> None:
    real = os.path.realpath(path)
    for base in {os.path.realpath(root), os.path.realpath(DEFAULT_ROOT)}:
        for rel in FORBIDDEN_DIRS:
            for forbidden in {os.path.join(base, rel), os.path.realpath(os.path.join(base, rel))}:  # the directory may be a link
                if real == forbidden or real.startswith(forbidden + os.sep):
                    raise EditError(2, "usage", f"the {what} file {path} is inside {rel}, which AI agents must not open "
                                    "(see CONSTRAINTS.md). It was not opened.")


def _read_arg(value: str | None, path: str | None, what: str, root: Path) -> bytes:
    if value is not None:
        return value.encode("utf-8", "surrogateescape")
    _refuse_forbidden(path, root, what)
    try:
        return Path(path).read_bytes()
    except OSError as e:
        raise EditError(2, "usage", f"cannot read the {what} file {path}: {e.strerror}.") from e


def main(argv: list[str] | None = None, root: Path | None = None) -> int:
    parser = argparse.ArgumentParser(prog="shared_edit.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("file", nargs="?", help="a shared file (relative to .local)")
    parser.add_argument("--old")
    parser.add_argument("--old-file")
    parser.add_argument("--new")
    parser.add_argument("--new-file")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--help-full", action="store_true", help="print the rules of the tool and exit")
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 0 if e.code == 0 else 2
    if args.help_full:
        print(__doc__)
        return 0
    try:
        if not args.file or (args.old is None) == (args.old_file is None) \
                or (args.new is None) == (args.new_file is None):
            raise EditError(2, "usage", "give FILE, exactly one of --old/--old-file and exactly one of "
                            "--new/--new-file (--new '' deletes the text).")
        root = root or DEFAULT_ROOT
        old = _read_arg(args.old, args.old_file, "--old", root)
        new = _read_arg(args.new, args.new_file, "--new", root)
        rel, before, after = edit_file(root, args.file, old, new, args.timeout)
    except EditError as e:
        print(f"shared-edit: FAIL {e.kind}: {e.message}", file=sys.stderr)
        return e.code
    print(f"shared-edit: ok {rel} ({before} -> {after} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
