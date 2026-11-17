#!/usr/bin/env python3
"""Mutation check of test_shared_edit.py: each mutation of shared_edit.py must make the tests fail.

  python3 tools/knowledge/mutations.py          run all
  python3 tools/knowledge/mutations.py --check  only check that every place to mutate is found once

Each mutation is a list of (old, new) replacements applied to a copy of shared_edit.py in a
temporary directory; the real file is never changed.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = (HERE / "shared_edit.py").read_text(encoding="utf-8")

MUTATIONS = [
    ("lock the shared file itself instead of the lock file (the file is replaced by a rename)",
     [("lock_fd = _acquire(root / LOCK_NAME, timeout)", "lock_fd = _acquire(target, timeout)")]),
    ("read the file before taking the lock",
     [("        lock_fd = _acquire(root / LOCK_NAME, timeout)\n    except OSError",
       "        pre = open(target, 'rb').read()\n        lock_fd = _acquire(root / LOCK_NAME, timeout)\n    except OSError"),
      ("            data = f.read()", "            data = pre")]),
    ("replace the first of several matches", [("if data.find(old, first + 1) >= 0:", "if False:")]),
    ("write a missing match as if it were at the end", [("if first < 0:", "if False:")]),
    ("write the file in place instead of renaming", [("os.replace(tmp, target)", "open(target, 'wb').write(result)")]),
    ("delete the lock file when the lock is released", [("        os.close(lock_fd)\n\n\ndef _refuse_forbidden", "        os.unlink(root / LOCK_NAME)\n        os.close(lock_fd)\n\n\ndef _refuse_forbidden")]),
    ("put the temporary file outside the directory of the shared file", [("tmp_path = target.with_name(", "tmp_path = Path('/dev/shm').joinpath(")]),
    ("release the lock before the rename", [("os.replace(tmp, target)", "fcntl.flock(lock_fd, fcntl.LOCK_UN); os.replace(tmp, target)")]),
    ("write any file inside .local", [("if rel in SHARED_FILES:", "if True:")]),
    ("follow a symbolic link", [("or os.path.islink(target) \\\n                or os.path.realpath(target) != os.path.join(os.path.realpath(root), rel):", ":")]),
    ("lose the permissions of the file", [("os.fchmod(f.fileno(), st.st_mode & 0o7777)", "pass")]),
    ("accept an empty text to replace", [("    if not old:\n", "    if False:\n")]),
    ("accept a NaN, infinite or negative timeout", [("if not math.isfinite(timeout) or timeout < 0:", "if False:")]),
    ("delete a temporary file that this call did not create",
     [("        tmp_fd = os.open(tmp_path,", "        tmp = tmp_path\n        tmp_fd = os.open(tmp_path,")]),
    ("read --old-file from a forbidden directory", [("    _refuse_forbidden(path, root, what)\n", "")]),
    ("forget to remove the temporary file after a failure", [("            try:\n                os.unlink(tmp)\n            except OSError:\n                pass\n", "            pass\n")]),
]


def apply(replacements: list[tuple[str, str]]) -> str:
    text = SOURCE
    for old, new in replacements:
        if text.count(old) != 1:
            raise SystemExit(f"mutations: the text to mutate is found {text.count(old)} times, not once: {old[:60]!r}")
        text = text.replace(old, new)
    return text


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    survived = []
    for name, replacements in MUTATIONS:
        mutated = apply(replacements)
        if check_only:
            continue
        with tempfile.TemporaryDirectory(prefix="shared-edit-mutation-") as d:
            (Path(d) / "shared_edit.py").write_text(mutated, encoding="utf-8")
            env = dict(os.environ, SHARED_EDIT_DIR=d, SHARED_EDIT_FAST="1")
            r = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(HERE), "-p", "test_shared_edit.py"],
                               env=env, capture_output=True, text=True, timeout=600)
        caught = r.returncode != 0
        print(f"  {'caught  ' if caught else 'SURVIVED'} {name}")
        if not caught:
            survived.append(name)
    if check_only:
        print(f"mutations: {len(MUTATIONS)} places found")
        return 0
    print(f"mutations: {len(MUTATIONS) - len(survived)} of {len(MUTATIONS)} caught" + (f"; survived: {survived}" if survived else ""))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
