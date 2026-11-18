# SPDX-License-Identifier: MIT
"""Tests of scripts/check-commits.py: which commit titles pass, which files must not be committed, and what
--message and --changed (the checks that CI makes of a pull request) say on a made-up repository.

    python tests/scripts/test_check_commits.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_commits", ROOT / "scripts" / "check-commits.py")
check_commits = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check_commits)


def codes(title: str) -> list[str]:
    return [code for code, _ in check_commits.message_problems(title)]


class Titles(unittest.TestCase):
    def test_good_titles(self):
        for title in ("feat: add LIN signal encoding", "fix(j1939): keep the frame order within a step",
                      "docs(ci,gui): say what the gate reads", "feat!: drop the old flag",
                      "Merge branch 'main' into x"):
            self.assertEqual(codes(title), [], title)

    def test_bad_titles(self):
        cases = {
            "Add LIN signal encoding": ["title-format"],
            "feature: add it": ["title-type"],
            "fix(J1939): x": ["title-scope"],
            "fix:": ["title-description"],
            "fix: ": ["title-description"],
            "": ["empty-message"],
        }
        for title, expected in cases.items():
            self.assertEqual(codes(title), expected, repr(title))

    def test_second_line_is_blank(self):
        self.assertEqual(codes("fix: x\nwhy"), ["second-line"])
        self.assertEqual(codes("fix: x\n\nwhy"), [])


class Paths(unittest.TestCase):
    def test_files_that_must_not_be_committed(self):
        for path in (".local/plans/x.md", ".local", "CLAUDE.local.md", "build/logs/x.log"):
            self.assertEqual(len(check_commits.path_problems(path, "remove it")), 1, path)

    def test_files_that_may_be(self):
        for path in ("docs/local.md", "scripts/build/x.py", "tools/knowledge/templates/.local-note.md", "CLAUDE.md"):
            self.assertEqual(check_commits.path_problems(path, "remove it"), [], path)


class Changed(unittest.TestCase):
    """--changed on a repository with a base commit and a branch: only the files of the difference count."""

    def run_changed(self, repo: Path, spec: str) -> tuple[int, str]:
        out = io.StringIO()
        with unittest.mock.patch.object(check_commits, "ROOT", repo), \
                unittest.mock.patch.object(sys, "argv", ["check-commits.py", "--changed", spec]), \
                contextlib.redirect_stdout(out):
            code = check_commits.main()
        return code, out.getvalue()

    def make(self, root: Path, files: list[str], removed: list[str] = ()) -> None:
        def git(*args):
            subprocess.run(["git", *args], cwd=root, check=True, capture_output=True)
        git("init", "-q", "-b", "main")
        git("config", "user.email", "t@example.com")
        git("config", "user.name", "t")
        (root / "base.txt").write_text("x")
        for name in removed:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text("x")
        git("add", "-f", ".")
        git("commit", "-q", "-m", "base")
        for name in files:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text("y")
        for name in removed:
            (root / name).unlink()
        git("add", "-A", "-f", ".")
        git("commit", "-q", "-m", "change")

    def test_clean_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.make(Path(tmp), ["docs/a.md", "scripts/b.py"])
            code, out = self.run_changed(Path(tmp), "HEAD^1..HEAD")
        self.assertEqual(code, 0, out)
        self.assertIn("ok (2 changed files)", out)

    def test_local_files_are_named(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.make(Path(tmp), ["docs/a.md", ".local/plans/p.md", "CLAUDE.local.md"])
            code, out = self.run_changed(Path(tmp), "HEAD^1..HEAD")
        self.assertEqual(code, 1)
        self.assertIn("local-file: .local/plans/p.md is local-only", out)
        self.assertIn("local-file: CLAUDE.local.md holds private instructions", out)
        self.assertNotIn("docs/a.md", out)
        self.assertIn("2 problems", out)

    def test_a_deleted_local_file_is_not_a_problem(self):
        # The change removes a file that an earlier commit added by mistake: that is the fix, not a new problem.
        with tempfile.TemporaryDirectory() as tmp:
            self.make(Path(tmp), [], removed=[".local/old.md"])
            code, out = self.run_changed(Path(tmp), "HEAD^1..HEAD")
        self.assertEqual(code, 0, out)


class MessageFile(unittest.TestCase):
    def test_title_from_a_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "title.txt"
            for text, expected in (("fix(ci): gate the merge\n", 0), ("Gate the merge\n", 1)):
                path.write_text(text)
                out = io.StringIO()
                with unittest.mock.patch.object(sys, "argv", ["check-commits.py", "--message", str(path)]), \
                        contextlib.redirect_stdout(out):
                    self.assertEqual(check_commits.main(), expected, text)
                if expected:
                    self.assertIn("message: title-format:", out.getvalue())


if __name__ == "__main__":
    unittest.main()
