#!/usr/bin/env python3
"""Tests of init_local.py (temporary directories only).  python3 -m unittest discover -s tools/knowledge"""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import init_local  # noqa: E402


class InitLocal(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="init-local-test-")
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        (self.repo / ".git" / "info").mkdir(parents=True)

    def run_init(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = init_local.main(["--repo", str(self.repo), *args])
        return code, out.getvalue()

    def run_init_at(self, repo, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = init_local.main(["--repo", str(repo), *args])
        return code, out.getvalue()

    def test_dry_run_writes_nothing(self):
        code, out = self.run_init()
        self.assertEqual(code, 0)
        self.assertIn("would create", out)
        self.assertFalse((self.repo / ".local").exists())
        self.assertFalse((self.repo / "CLAUDE.local.md").exists())

    def test_apply_creates_the_layout_and_excludes(self):
        code, _ = self.run_init("--apply")
        self.assertEqual(code, 0)
        for rel in ("CONSTRAINTS.md", "plans/STATE.md", "plans/RULES.md", "plans/FLAKY.md", "decisions/ACTIVE.md",
                    "changelog/pending.md"):
            self.assertTrue((self.repo / ".local" / rel).is_file(), rel)
        self.assertTrue((self.repo / ".local/plans/streams").is_dir())
        self.assertTrue((self.repo / "CLAUDE.local.md").is_file())
        lines = (self.repo / ".git/info/exclude").read_text().splitlines()
        self.assertIn("/.local/", lines)
        self.assertIn("/CLAUDE.local.md", lines)

    def test_existing_files_are_never_overwritten(self):
        (self.repo / ".local/plans").mkdir(parents=True)
        (self.repo / ".local/plans/STATE.md").write_text("MINE\n")
        (self.repo / "CLAUDE.local.md").write_text("MY INSTRUCTIONS\n")
        (self.repo / ".git/info/exclude").write_text("*.o")  # no trailing newline
        code, out = self.run_init("--apply")
        self.assertEqual(code, 0)
        self.assertEqual((self.repo / ".local/plans/STATE.md").read_text(), "MINE\n")
        self.assertEqual((self.repo / "CLAUDE.local.md").read_text(), "MY INSTRUCTIONS\n")
        self.assertIn("keep plans/STATE.md", out)
        self.assertEqual((self.repo / ".git/info/exclude").read_text().splitlines()[:2], ["*.o", "/.local/"])

    def test_second_run_changes_nothing(self):
        self.run_init("--apply")
        before = {p: p.read_bytes() for p in self.repo.rglob("*") if p.is_file()}
        _, out = self.run_init("--apply")
        self.assertEqual(before, {p: p.read_bytes() for p in self.repo.rglob("*") if p.is_file()})
        self.assertNotIn("create ", out.replace("would create", ""))

    def test_not_a_repository_is_a_usage_error(self):
        other = tempfile.TemporaryDirectory(prefix="init-local-nogit-")
        self.addCleanup(other.cleanup)
        out = io.StringIO()
        with contextlib.redirect_stderr(out):
            self.assertEqual(init_local.main(["--repo", other.name, "--apply"]), 2)
        self.assertFalse((Path(other.name) / ".local").exists())

    def test_linked_worktree_uses_the_main_checkout(self):
        wt = self.repo.parent / (self.repo.name + "-wt")
        self.addCleanup(lambda: __import__("shutil").rmtree(wt, ignore_errors=True))
        gitdir = self.repo / ".git" / "worktrees" / "wt"
        gitdir.mkdir(parents=True)
        (gitdir / "commondir").write_text("../..\n")
        wt.mkdir()
        (wt / ".git").write_text(f"gitdir: {gitdir}\n")
        code, _ = self.run_init_at(wt, "--apply")
        self.assertEqual(code, 0)
        self.assertTrue((self.repo / ".local/plans/STATE.md").is_file())
        self.assertFalse((wt / ".local").exists())
        self.assertFalse((wt / "CLAUDE.local.md").exists())
        self.assertIn("/.local/", (self.repo / ".git/info/exclude").read_text())

    def test_relative_gitdir_is_resolved_from_the_working_tree(self):
        wt = self.repo / "sub-wt"
        gitdir = self.repo / ".git" / "worktrees" / "wt"
        gitdir.mkdir(parents=True)
        (gitdir / "commondir").write_text("../..\n")
        wt.mkdir()
        (wt / ".git").write_text("gitdir: ../.git/worktrees/wt\n")
        self.assertEqual(init_local.git_dir(wt), (self.repo / ".git").resolve())

    def test_symbolic_link_is_refused_and_nothing_is_written_through_it(self):
        outside = Path(self._tmp.name + "-outside")
        outside.mkdir()
        self.addCleanup(lambda: __import__("shutil").rmtree(outside, ignore_errors=True))
        (self.repo / ".local").symlink_to(outside)
        code, _ = self.run_init("--apply")
        self.assertEqual(code, 2)
        self.assertEqual(list(outside.iterdir()), [])

    def test_templates_have_no_absolute_paths_or_real_knowledge(self):
        for t in (Path(init_local.__file__).parent / "templates").iterdir():
            text = t.read_text(encoding="utf-8")
            self.assertNotIn("/home/", text, t.name)
            self.assertNotIn("/Users/", text, t.name)


if __name__ == "__main__":
    unittest.main()
