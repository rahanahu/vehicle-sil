#!/usr/bin/env python3
"""Tests of shared_edit.py. They use temporary directories only and never touch the real .local.

  python3 -m unittest discover -s tools/knowledge

Nothing here depends on timing: a child process tells the test where it stands by creating a marker
file, and the test waits for that marker (never for a fixed time). SHARED_EDIT_DIR names the
directory that holds the shared_edit.py to test (mutations.py uses it); SHARED_EDIT_FAST=1 shortens
the concurrency tests.
"""

from __future__ import annotations

import contextlib
import errno
try:
    import fcntl
except ImportError:  # not POSIX: shared_edit.py needs fcntl
    import unittest
    raise unittest.SkipTest("shared_edit.py needs a POSIX system")
import hashlib
import io
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
TOOL_DIR = Path(os.environ.get("SHARED_EDIT_DIR", HERE)).resolve()
sys.path.insert(0, str(TOOL_DIR))
import shared_edit  # noqa: E402

FAST = os.environ.get("SHARED_EDIT_FAST") == "1"
MARKER_WAIT = 10.0
END = "<!--END-->"

RUNNER = ("import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); import shared_edit; "
          "sys.exit(shared_edit.main(sys.argv[3:], root=Path(sys.argv[2])))")

# A child that edits plans/STATE.md. `mode` makes it stop at a chosen point; it reports that it is
# waiting for the lock by creating <marker>.waiting.
CHILD = """
import fcntl, os, sys, time
from pathlib import Path
sys.path.insert(0, sys.argv[1]); import shared_edit
root, mode, marker = Path(sys.argv[2]), sys.argv[3], sys.argv[4]
old, new = sys.argv[5].encode(), sys.argv[6].encode()
real_flock, real_replace = fcntl.flock, os.replace
def flock(fd, op):
    try:
        return real_flock(fd, op)
    except BlockingIOError:
        Path(marker + '.waiting').touch()
        raise
fcntl.flock = flock
if mode == 'before-replace':
    def rep(a, b):
        Path(marker).touch(); time.sleep(3600)
    os.replace = rep
elif mode == 'after-replace':
    def rep(a, b):
        real_replace(a, b); Path(marker).touch(); time.sleep(3600)
    os.replace = rep
shared_edit.edit_file(root, 'plans/STATE.md', old, new, 60)
"""

def worker_source() -> str:
    return (
        "import os, sys, time\n"
        "from pathlib import Path\n"
        "sys.path.insert(0, sys.argv[1]); import shared_edit\n"
        "root, go, wid, count = Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], int(sys.argv[5])\n"
        "while not go.exists():\n"
        "    time.sleep(0.005)\n"
        "for i in range(count):\n"
        f"    shared_edit.edit_file(root, 'plans/STATE.md', b'{END}', f'w{{wid}}-{{i}}\\n{END}'.encode(), 120)\n"
    )


def wait_for(path: Path, what: str) -> None:
    deadline = time.monotonic() + MARKER_WAIT
    while not path.exists():
        if time.monotonic() > deadline:
            raise AssertionError(f"the marker for '{what}' did not appear within {MARKER_WAIT:g} s")
        time.sleep(0.01)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class LocalDir(unittest.TestCase):
    def test_a_linked_working_tree_uses_the_main_checkouts_local_dir(self):
        with tempfile.TemporaryDirectory(prefix="shared-edit-wt-") as tmp:
            main = Path(tmp) / "main"
            gitdir = main / ".git" / "worktrees" / "wt"
            gitdir.mkdir(parents=True)
            (gitdir / "commondir").write_text("../..\n")
            wt = Path(tmp) / "wt"
            (wt / "tools/knowledge").mkdir(parents=True)
            (wt / ".git").write_text("gitdir: ../main/.git/worktrees/wt\n")  # relative, as newer git writes it
            self.assertEqual(shared_edit._local_dir(wt / "tools/knowledge/shared_edit.py"), main.resolve() / ".local")
            (main / "tools/knowledge").mkdir(parents=True)
            (main / ".git").mkdir(exist_ok=True)
            self.assertEqual(shared_edit._local_dir(main / "tools/knowledge/shared_edit.py"), main.resolve() / ".local")


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="shared-edit-test-")
        self.addCleanup(self._tmp.cleanup)
        self.base = Path(self._tmp.name)
        self.root = self.base / ".local"
        for rel in shared_edit.SHARED_FILES:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
        self.state = self.root / "plans/STATE.md"
        self.state.write_text(f"# state\nline one\nline two\n{END}\n", encoding="utf-8")
        for rel in shared_edit.SHARED_FILES:
            if not (self.root / rel).exists():
                (self.root / rel).write_text(f"# {rel}\n", encoding="utf-8")
        self.procs: list[subprocess.Popen] = []
        self.addCleanup(self._reap)

    def _reap(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
            for stream in (p.stdout, p.stderr):
                if stream:
                    stream.close()

    def edit(self, old: str, new: str, name: str = "plans/STATE.md", timeout: float = 5):
        return shared_edit.edit_file(self.root, name, old.encode(), new.encode(), timeout)

    def cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, "-c", RUNNER, str(TOOL_DIR), str(self.root), *args],
                              capture_output=True, text=True, timeout=120)

    def child(self, mode: str, marker: Path, old: str, new: str) -> subprocess.Popen:
        p = subprocess.Popen([sys.executable, "-c", CHILD, str(TOOL_DIR), str(self.root), mode, str(marker), old, new],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def hold_lock(self) -> int:
        fd = os.open(self.root / shared_edit.LOCK_NAME, os.O_RDWR | os.O_CREAT, 0o600)
        fcntl.flock(fd, fcntl.LOCK_EX)
        self.addCleanup(lambda: os.close(fd) if self._open(fd) else None)
        return fd

    @staticmethod
    def _open(fd: int) -> bool:
        try:
            os.fstat(fd)
            return True
        except OSError:
            return False

    def leftovers(self) -> list[str]:
        return sorted(p.name for p in self.root.rglob(".*.tmp"))


class Replace(Base):
    def test_t1_replace_add_delete_keep_the_rest_byte_for_byte(self):
        before = self.state.read_bytes()
        self.edit("line one", "line 1")
        self.assertEqual(self.state.read_bytes(), before.replace(b"line one", b"line 1"))
        self.edit("line two\n", "line two\nline three\n")
        self.assertIn(b"line two\nline three\n" + END.encode(), self.state.read_bytes())
        self.edit("line three\n", "")
        self.assertEqual(self.state.read_bytes(), before.replace(b"line one", b"line 1"))
        self.assertEqual(self.leftovers(), [])

    def test_t2_text_not_found_changes_nothing(self):
        digest = sha(self.state)
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("no such text", "x")
        self.assertEqual((cm.exception.code, cm.exception.kind), (1, "no-match"))
        self.assertEqual(sha(self.state), digest)
        self.assertEqual(self.leftovers(), [])

    def test_t3_text_found_twice_changes_nothing(self):
        self.state.write_text("aaa\nline\nline\n", encoding="utf-8")
        digest = sha(self.state)
        for old in ("line", "aa"):  # "aa" occurs twice, overlapping
            with self.assertRaises(shared_edit.EditError) as cm:
                self.edit(old, "x")
            self.assertEqual((cm.exception.code, cm.exception.kind), (1, "ambiguous"))
        self.assertEqual(sha(self.state), digest)

    def test_t11_two_writers_change_the_same_line_the_second_fails(self):
        self.edit("line one", "line A")
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "line B")
        self.assertEqual(cm.exception.kind, "no-match")
        self.assertIn(b"line A", self.state.read_bytes())
        self.assertNotIn(b"line B", self.state.read_bytes())

    def test_t12_bytes_and_permissions_are_kept(self):
        data = "日本語\r\n二行目\r\nend without newline".encode()
        self.state.write_bytes(data)
        os.chmod(self.state, 0o640)
        self.edit("二行目", "２行目")
        self.assertEqual(self.state.read_bytes(), data.replace("二行目".encode(), "２行目".encode()))
        self.assertEqual(self.state.stat().st_mode & 0o7777, 0o640)

    def test_t15_io_error_changes_nothing_and_leaves_no_file(self):
        if os.geteuid() == 0:
            self.skipTest("root ignores directory permissions")
        digest = sha(self.state)
        os.chmod(self.state.parent, 0o555)
        self.addCleanup(os.chmod, self.state.parent, 0o755)
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "x")
        self.assertEqual((cm.exception.code, cm.exception.kind), (1, "io-error"))
        self.assertEqual(sha(self.state), digest)
        self.assertEqual(self.leftovers(), [])


    def test_t15_a_failed_rename_removes_the_temporary_file_and_frees_the_lock(self):
        digest = sha(self.state)
        with mock.patch.object(shared_edit.os, "replace", side_effect=OSError(errno.ENOSPC, "No space left on device")):
            with self.assertRaises(shared_edit.EditError) as cm:
                self.edit("line one", "x")
        self.assertEqual((cm.exception.code, cm.exception.kind), (1, "io-error"))
        self.assertEqual(sha(self.state), digest)
        self.assertEqual(self.leftovers(), [])
        self.edit("line one", "x", timeout=0.3)


class Targets(Base):
    def test_t10_only_the_five_shared_files_are_written(self):
        other = self.root / "plans/OTHER.md"
        other.write_text("x\n")
        (self.root / "README.md").write_text("x\n")
        for name in ("plans/OTHER.md", "README.md", "../x.md", "plans/../README.md", "/etc/hostname", "plans/STATE.md.bak"):
            with self.assertRaises(shared_edit.EditError, msg=name) as cm:
                self.edit("x", "y", name=name)
            self.assertEqual(cm.exception.code, 2, name)
        self.assertEqual(other.read_text(), "x\n")

    def test_t10_names_are_accepted_relative_to_local_to_the_repository_and_absolute(self):
        for name in ("plans/STATE.md", ".local/plans/STATE.md", str(self.state)):
            self.edit("line one", "line one!", name=name)
            self.edit("line one!", "line one", name=name)

    def test_t10_a_missing_file_is_not_created(self):
        (self.root / "decisions/ACTIVE.md").unlink()
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("x", "y", name="decisions/ACTIVE.md")
        self.assertEqual(cm.exception.code, 2)
        self.assertFalse((self.root / "decisions/ACTIVE.md").exists())

    def test_t10_symbolic_links_and_directories_are_refused(self):
        real = self.root / "plans/real.txt"
        real.write_text("line one\n")
        self.state.unlink()
        self.state.symlink_to(real)
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "y")
        self.assertEqual(cm.exception.code, 2)
        self.assertEqual(real.read_text(), "line one\n")
        self.state.unlink()
        self.state.mkdir()
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "y")
        self.assertEqual(cm.exception.code, 2)

    def test_t10_a_linked_directory_in_the_path_is_refused(self):
        outside = self.base / "outside"
        outside.mkdir()
        (outside / "RULES.md").write_text("line one\n")
        for p in (self.root / "plans/RULES.md",):
            p.unlink()
        (self.root / "plans").rename(self.root / "plans-real")
        (self.root / "plans").symlink_to(outside)
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "y", name="plans/RULES.md")
        self.assertEqual(cm.exception.code, 2)
        self.assertEqual((outside / "RULES.md").read_text(), "line one\n")

    def test_t13_usage_errors_exit_with_2(self):
        for args in ((), ("plans/STATE.md",), ("plans/STATE.md", "--old", "", "--new", "x"),
                     ("plans/STATE.md", "--old", "x", "--new", "x"),
                     ("plans/STATE.md", "--old", "x"),
                     ("plans/STATE.md", "--old", "x", "--old-file", "/dev/null", "--new", "y"),
                     ("plans/STATE.md", "--old-file", "/no/such/file", "--new", "y"),
                     ("--bogus",)):
            r = self.cli(*args)
            self.assertEqual(r.returncode, 2, (args, r.stderr))
            self.assertEqual(r.stdout, "")

    def test_t13_cli_success_failure_and_files(self):
        r = self.cli("plans/STATE.md", "--old", "line one", "--new", "line 1")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"^shared-edit: ok plans/STATE.md \(\d+ -> \d+ bytes\)\n$")
        old, new = self.base / "old.txt", self.base / "new.txt"
        old.write_text("line two\nsecond\n")
        new.write_text("")
        self.assertEqual(self.cli("plans/STATE.md", "--old-file", str(old), "--new-file", str(new)).returncode, 1)
        old.write_text("line two\n")
        self.assertEqual(self.cli("plans/STATE.md", "--old-file", str(old), "--new-file", str(new)).returncode, 0)
        self.assertNotIn(b"line two", self.state.read_bytes())
        r = self.cli("plans/STATE.md", "--old", "gone", "--new", "x")
        self.assertEqual(r.returncode, 1)
        self.assertIn("shared-edit: FAIL no-match:", r.stderr)
        self.assertEqual(r.stdout, "")


class Hardening(Base):
    def test_t16_timeout_must_be_a_finite_number_of_seconds_0_or_more(self):
        digest = sha(self.state)
        for value in (float("nan"), float("inf"), float("-inf"), -1.0, -0.001):
            with self.assertRaises(shared_edit.EditError, msg=str(value)) as cm:
                self.edit("line one", "x", timeout=value)
            self.assertEqual((cm.exception.code, cm.exception.kind), (2, "usage"), str(value))
        for text in ("nan", "inf", "-inf", "-1"):
            r = self.cli("plans/STATE.md", "--old", "line one", "--new", "x", f"--timeout={text}")
            self.assertEqual(r.returncode, 2, (text, r.stderr))
            self.assertIn("--timeout", r.stderr)
            self.assertEqual(r.stdout, "")
        self.assertEqual(sha(self.state), digest)
        self.edit("line one", "ok", timeout=0)  # 0 means: try once

    def test_t17_a_temporary_file_that_this_call_did_not_create_is_never_deleted(self):
        foreign = self.state.parent / f".STATE.md.{os.getpid()}.00000001.tmp"
        foreign.write_text("someone else's file\n")
        digest = sha(self.state)
        with mock.patch.object(shared_edit.os, "urandom", return_value=bytes([0, 0, 0, 1])):
            with self.assertRaises(shared_edit.EditError) as cm:
                self.edit("line one", "x")
        self.assertEqual((cm.exception.code, cm.exception.kind), (1, "io-error"))
        self.assertEqual(foreign.read_text(), "someone else's file\n")
        self.assertEqual(sha(self.state), digest)
        self.edit("line one", "x", timeout=0.3)  # the lock was released

    def forbidden_paths(self) -> list[Path]:
        vendor_real = self.root / "vendor/download 1.0"  # the real directory is a link target, as in .local/vendor
        vendor_link = self.root / "vendor/third-party"
        vendor_real.mkdir(parents=True)
        vendor_link.symlink_to(vendor_real)
        (vendor_real / "secret.txt").write_text("SECRET-VENDOR\n")
        link = self.base / "link.txt"
        link.symlink_to(vendor_link / "secret.txt")
        return [vendor_link / "_probe-nonexistent.h", vendor_real / "_probe-nonexistent.h", vendor_real / "secret.txt",
                vendor_link / "secret.txt", link, shared_edit.DEFAULT_ROOT / "vendor/third-party/_probe-nonexistent.h"]

    def test_t18_old_file_and_new_file_are_refused_before_they_are_opened_inside_the_forbidden_directories(self):
        digest = sha(self.state)
        for path in self.forbidden_paths():
            for flag in ("--old-file", "--new-file"):
                args = ["plans/STATE.md", flag, str(path)] + (["--new", "x"] if flag == "--old-file" else ["--old", "line one"])
                with mock.patch.object(Path, "read_bytes") as read_bytes, mock.patch("builtins.open") as opened, \
                        mock.patch.object(shared_edit.os, "open") as os_open:
                    err = io.StringIO()
                    with contextlib.redirect_stderr(err):
                        code = shared_edit.main(args, root=self.root)
                self.assertEqual(code, 2, (str(path), flag))
                self.assertIn("It was not opened.", err.getvalue())
                self.assertNotIn("SECRET-VENDOR", err.getvalue())
                read_bytes.assert_not_called()
                opened.assert_not_called()
                os_open.assert_not_called()
        self.assertEqual(sha(self.state), digest)

    def test_t18_other_files_are_still_read(self):
        old, new = self.base / "old.txt", self.base / "new.txt"
        old.write_text("line one")
        new.write_text("line 1")
        self.assertEqual(self.cli("plans/STATE.md", "--old-file", str(old), "--new-file", str(new)).returncode, 0)
        self.assertIn(b"line 1", self.state.read_bytes())


class Locking(Base):
    def test_t4_no_update_is_lost_by_concurrent_writers(self):
        workers, count = (4, 50) if FAST else (8, 1000)
        go = self.base / "go"
        procs = [subprocess.Popen([sys.executable, "-c", worker_source(), str(TOOL_DIR), str(self.root), str(go), str(w), str(count)],
                                  stderr=subprocess.PIPE) for w in range(workers)]
        self.procs += procs
        cli = [subprocess.Popen([sys.executable, "-c", RUNNER, str(TOOL_DIR), str(self.root), "plans/STATE.md",
                                 "--old", END, "--new", f"cli{c}\n{END}"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
               for c in range(10)]
        self.procs += cli
        go.touch()
        for p in procs + cli:
            _, err = p.communicate(timeout=300)
            self.assertEqual(p.returncode, 0, err)
        lines = self.state.read_text().splitlines()
        expected = {f"w{w}-{i}" for w in range(workers) for i in range(count)} | {f"cli{c}" for c in range(10)}
        self.assertEqual(len(lines), 3 + len(expected) + 1)  # 3 original lines, the new lines, the marker
        self.assertEqual(set(lines[3:-1]), expected)
        self.assertEqual(lines[-1], END)
        self.assertEqual(self.leftovers(), [])

    def test_t5_a_writer_that_waited_reads_the_file_that_replaced_it(self):
        # The file is replaced by another writer while this one waits for the lock.
        fd = self.hold_lock()
        marker = self.base / "marker"
        p = self.child("none", marker, END, f"from child\n{END}")
        wait_for(Path(str(marker) + ".waiting"), "child waits for the lock")
        replacement = self.base / "replacement"
        replacement.write_text(f"# state\nwritten while the child waited\n{END}\n")
        os.replace(replacement, self.state)
        fcntl.flock(fd, fcntl.LOCK_UN)
        _, err = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 0, err)
        self.assertEqual(self.state.read_text(), f"# state\nwritten while the child waited\nfrom child\n{END}\n")

    def test_t8_lock_timeout_changes_nothing_and_the_same_edit_works_when_the_lock_is_free(self):
        fd = self.hold_lock()
        digest = sha(self.state)
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line one", "x", timeout=0.3)
        self.assertEqual((cm.exception.code, cm.exception.kind), (1, "lock-timeout"))
        self.assertEqual(sha(self.state), digest)
        r = self.cli("plans/STATE.md", "--old", "line one", "--new", "x", "--timeout", "0.3")
        self.assertEqual(r.returncode, 1)
        self.assertIn("FAIL lock-timeout:", r.stderr)
        fcntl.flock(fd, fcntl.LOCK_UN)
        self.edit("line one", "x")
        self.assertIn(b"\nx\n", self.state.read_bytes())

    def test_t9_the_lock_is_released_after_every_outcome_and_the_lock_file_stays(self):
        self.edit("line one", "line 1")
        lock = self.root / shared_edit.LOCK_NAME
        inode = lock.stat().st_ino
        for old, new in (("no such", "x"), ("line", "x"), ("line 1", "line 1")):
            with self.assertRaises(shared_edit.EditError):
                self.edit(old, new, timeout=0.3)
        self.edit("line 1", "line one", timeout=0.3)
        self.assertEqual(lock.stat().st_ino, inode)

    def test_t6_a_writer_killed_before_the_rename_leaves_the_old_file_and_frees_the_lock(self):
        digest = sha(self.state)
        marker = self.base / "marker"
        p = self.child("before-replace", marker, "line one", "line killed")
        wait_for(marker, "child is about to rename")
        # The lock is held until the rename is done.
        with self.assertRaises(shared_edit.EditError) as cm:
            self.edit("line two", "x", timeout=0.3)
        self.assertEqual(cm.exception.kind, "lock-timeout")
        self.assertEqual(sha(self.state), digest)
        tmps = self.leftovers()
        self.assertEqual(len(tmps), 1)
        self.assertRegex(tmps[0], r"^\.STATE\.md\.\d+\.[0-9a-f]+\.tmp$")
        p.send_signal(signal.SIGKILL)
        p.wait()
        self.assertEqual(sha(self.state), digest)
        self.edit("line two", "line 2", timeout=2)
        self.assertEqual(sorted(q.name for q in self.root.rglob("*.md") if q.name.endswith(".tmp")), [])
        self.assertNotIn(b"line killed", self.state.read_bytes())

    def test_t7_a_writer_killed_after_the_rename_leaves_the_whole_new_file(self):
        marker = self.base / "marker"
        p = self.child("after-replace", marker, "line one", "line done")
        wait_for(marker, "child has renamed")
        p.send_signal(signal.SIGKILL)
        p.wait()
        self.assertEqual(self.state.read_text(), f"# state\nline done\nline two\n{END}\n")
        self.edit("line two", "line 2", timeout=2)
        self.assertEqual(self.leftovers(), [])


if __name__ == "__main__":
    unittest.main()
