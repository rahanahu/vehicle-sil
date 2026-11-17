# SPDX-License-Identifier: MIT
"""Tests of scripts/hotspots.py on made-up repositories: which files get which signal, what counts as a lower layer
(stack) and what as independent work (parallel), what is left out (merged, closed, stale), what the script says it
could not observe, and what it does when gh fails or hangs.

    python tests/scripts/test_hotspots.py      (exit code 0 when every test passes)
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import hotspots  # noqa: E402

NOW = datetime.now(timezone.utc)


def iso(days_ago: float = 0.0) -> str:
    return (NOW - timedelta(days=days_ago)).strftime("%Y-%m-%dT%H:%M:%S+00:00")


class Repo:
    def __init__(self, root: Path):
        self.root = root
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "commit.gpgsign", "false")

    def git(self, *args: str, date: str | None = None, cwd: Path | None = None) -> str:
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        if date:
            env |= {"GIT_AUTHOR_DATE": date, "GIT_COMMITTER_DATE": date}
        return subprocess.run(["git", *args], cwd=cwd or self.root, env=env, check=True, capture_output=True,
                              text=True).stdout.strip()

    def commit(self, files: dict[str, str | None], date: str | None = None, branch: str | None = None,
               start: str | None = None) -> str:
        if branch:
            self.git("checkout", "-q", "-B", branch, *([start] if start else []))
        for name, text in files.items():
            path = self.root / name
            if text is None:
                path.unlink()
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "c", date=date or iso(0.1))
        return self.git("rev-parse", "HEAD")


def lines(n: int, tag: str = "x") -> str:
    return "".join(f"{tag}{i}\n" for i in range(n))


def pr(number: int, head: str, sha: str, base: str = "main", state: str = "OPEN", merged: bool = False) -> dict:
    return {"number": number, "state": state, "headRefName": head, "headRefOid": sha, "baseRefName": base,
            "mergedAt": iso(0.5) if merged else None}


class HotspotsTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        (self.tmp / "repo").mkdir()
        self.repo = Repo(self.tmp / "repo")

    def fake_gh(self, code: str):
        return unittest.mock.patch.object(hotspots, "GH_COMMAND", [sys.executable, "-c", code])

    def run_tool(self, *args: str, prs: list[dict] | None = None, gh_code: str | None = None) -> tuple[dict, str]:
        """The result and the summary. Without prs and gh_code the pull requests are not asked for (--no-gh)."""
        argv = ["--repo", str(self.repo.root), "--base", "main", "--log", str(self.tmp / "h.log"), *args]
        if prs is None and gh_code is None:
            argv.append("--no-gh")
        if prs is not None:
            manifest = self.tmp / "prs.json"
            manifest.write_text(json.dumps(prs), encoding="utf-8")
            gh_code = ("import sys; a = sys.argv[1:]; "
                       "assert a[:4] == ['pr', 'list', '--state', 'all'] and '--limit' in a "
                       f"and a[a.index('--json') + 1] == {hotspots.GH_FIELDS!r}, a; "
                       f"sys.stdout.write(open({str(manifest)!r}).read())")
        parsed = hotspots.build_parser().parse_args(argv)
        with self.fake_gh(gh_code or "raise SystemExit(1)"):
            result = hotspots.analyse(parsed)
        return result, hotspots.render(result)[0]

    def file(self, data: dict, path: str) -> dict:
        return next(f for f in data["files"] if f["path"] == path)

    def test_size_and_churn_alone_are_information_not_attention(self):
        self.repo.commit({"big.cpp": lines(1599), "small.cpp": lines(10)}, date=iso(20))  # outside the churn window
        self.repo.commit({"big.cpp": lines(1600)}, date=iso(3))
        for i in range(4):
            self.repo.commit({"big.cpp": lines(1600 + i + 1)}, date=iso(2 - i * 0.1))
        data, text = self.run_tool("big.cpp", "small.cpp")
        big, small = self.file(data, "big.cpp"), self.file(data, "small.cpp")
        self.assertEqual(big["signals"], ["size", "churn"])
        self.assertFalse(big["attention"])
        self.assertEqual(data["large_busy"], ["big.cpp"])
        self.assertIn("large and busy, information only: big.cpp", text)
        self.assertEqual(big["churn_commits"], 5)
        self.assertEqual(small["signals"], [])

    def test_attention_needs_size_with_stack_or_parallel(self):
        self.repo.commit({"big.cpp": lines(1600), "small.cpp": lines(10)}, date=iso(9))
        for i in range(5):  # churn on both
            self.repo.commit({"big.cpp": lines(1601 + i), "small.cpp": lines(11 + i)}, date=iso(3 - i * 0.1))
        small = self.repo.commit({"small.cpp": lines(30)}, branch="a", date=iso(1), start="main")
        big = self.repo.commit({"big.cpp": lines(1700)}, branch="b", date=iso(1), start="main")
        self.repo.git("checkout", "-q", "main")
        data, _ = self.run_tool("big.cpp", "small.cpp", prs=[pr(1, "b", big)])
        self.assertEqual(data["attention"], ["big.cpp"])  # size with parallel
        data, _ = self.run_tool("big.cpp", "small.cpp", prs=[pr(2, "a", small)])
        self.assertEqual(data["attention"], [])  # small is busy and parallel but not large; big is large and busy but alone
        self.assertEqual(data["large_busy"], ["big.cpp"])
        self.assertTrue(self.file(data, "small.cpp")["parallel"])

    def test_size_threshold_is_inclusive(self):
        self.repo.commit({"at.cpp": lines(1500), "below.cpp": lines(1499)})
        data, _ = self.run_tool("at.cpp", "below.cpp")
        self.assertEqual(self.file(data, "at.cpp")["signals"], ["size"])
        self.assertEqual(self.file(data, "below.cpp")["signals"], [])

    def test_stack_is_not_parallel_and_own_change_excludes_lower_layers(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        a = self.repo.commit({"f.cpp": lines(1600) + lines(10, "a")}, branch="a", date=iso(1))
        b = self.repo.commit({"f.cpp": lines(1600) + lines(10, "a") + lines(3, "b")}, branch="b", date=iso(0.5))
        data, text = self.run_tool(prs=[pr(1, "a", a), pr(2, "b", b, base="a")])
        f = self.file(data, "f.cpp")
        self.assertEqual(f["own"][:2], [3, 0])
        self.assertEqual([(e["ref"], e["added"], e["state"]) for e in f["stack"]], [("a (#1)", 10, "open")])
        self.assertEqual(f["parallel"], [])
        self.assertTrue(f["attention"])
        self.assertIn("stack 1 lower layer(s) +10/-0", text)

    def test_with_paths_the_unmerged_part_of_head_is_a_lower_layer(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        a = self.repo.commit({"f.cpp": lines(1600) + lines(7, "a")}, branch="a", date=iso(1))
        data, _ = self.run_tool("f.cpp", prs=[pr(1, "a", a)])
        f = self.file(data, "f.cpp")
        self.assertEqual([(e["ref"], e["added"]) for e in f["stack"]], [("a (#1)", 7)])
        self.assertIsNone(f["own"])
        data, _ = self.run_tool(prs=[pr(1, "a", a)])
        self.assertEqual(self.file(data, "f.cpp")["stack"], [])  # without paths HEAD is the work itself

    def test_a_rewritten_lower_layer_of_the_pr_chain_is_still_lower(self):
        self.repo.commit({"f.cpp": lines(10)}, date=iso(9))
        old = self.repo.commit({"f.cpp": lines(11)}, branch="a", date=iso(2), start="main")
        self.repo.commit({"f.cpp": lines(11) + "up\n"}, branch="b", date=iso(1))
        new = self.repo.commit({"f.cpp": lines(11)}, branch="a", date=iso(1), start="main")  # a rewritten
        self.repo.git("checkout", "-q", "b")
        self.assertNotEqual(old, new)
        data, _ = self.run_tool(prs=[pr(1, "a", new), pr(2, "b", self.repo.git("rev-parse", "b"), base="a")])
        f = self.file(data, "f.cpp")
        self.assertEqual([e["ref"] for e in f["stack"]], ["a (#1)"])
        self.assertEqual(f["parallel"], [])
        self.assertEqual(f["own"][:2], [1, 0])  # the old copy of a's commit is patch-equivalent to a's new one

    def test_independent_open_work_is_parallel(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        a = self.repo.commit({"f.cpp": lines(1601)}, branch="a", date=iso(1), start="main")
        b = self.repo.commit({"f.cpp": lines(1602)}, branch="b", date=iso(1), start="main")
        self.repo.git("checkout", "-q", "a")
        data, _ = self.run_tool(prs=[pr(1, "a", a), pr(2, "b", b)])
        f = self.file(data, "f.cpp")
        self.assertEqual([e["ref"] for e in f["parallel"]], ["b (#2)"])
        self.assertEqual(f["stack"], [])
        self.assertTrue(f["attention"])

    def sibling_setup(self) -> str:
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        child = self.repo.commit({"f.cpp": lines(1600) + lines(30, "p")}, branch="p", date=iso(1), start="main")
        self.repo.git("checkout", "-q", "main")
        return child

    def test_before_the_work_a_branch_grown_from_head_is_parallel_not_upper(self):
        child = self.sibling_setup()
        data, _ = self.run_tool("f.cpp", prs=[pr(1, "p", child)])  # paths: nothing of the work exists yet
        f = self.file(data, "f.cpp")
        self.assertEqual([e["ref"] for e in f["parallel"]], ["p (#1)"])
        self.assertEqual(f["upper"], [])
        self.assertTrue(f["attention"])

    def test_at_the_base_with_nothing_committed_a_branch_grown_from_head_is_parallel(self):
        child = self.sibling_setup()
        (self.repo.root / "f.cpp").write_text(lines(1700), encoding="utf-8")  # work not committed yet; HEAD is the base
        data, _ = self.run_tool(prs=[pr(1, "p", child)])
        f = self.file(data, "f.cpp")
        self.assertEqual(([e["ref"] for e in f["parallel"]], f["upper"]), (["p (#1)"], []))

    def test_before_the_work_a_pr_stacked_on_head_is_parallel_too(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        low = self.repo.commit({"f.cpp": lines(1601)}, branch="low", date=iso(2), start="main")
        up = self.repo.commit({"f.cpp": lines(1602)}, branch="up", date=iso(1))
        self.repo.git("checkout", "-q", "low")
        data, _ = self.run_tool("f.cpp", prs=[pr(1, "low", low), pr(2, "up", up, base="low")])
        f = self.file(data, "f.cpp")
        self.assertEqual(([e["ref"] for e in f["stack"]], [e["ref"] for e in f["parallel"]]), (["low (#1)"], ["up (#2)"]))

    def test_a_branch_grown_from_the_work_stays_an_upper_layer(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        self.repo.commit({"f.cpp": lines(1601)}, branch="mine", date=iso(2), start="main")
        up = self.repo.commit({"f.cpp": lines(1602)}, branch="up", date=iso(1))
        self.repo.git("checkout", "-q", "mine")
        data, _ = self.run_tool(prs=[pr(1, "up", up, base="mine")])
        f = self.file(data, "f.cpp")
        self.assertEqual((f["parallel"], [e["ref"] for e in f["upper"]]), ([], ["up (#1)"]))

    def test_without_pr_data_nothing_is_counted_and_the_output_says_so(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        self.repo.commit({"f.cpp": lines(1601)}, branch="a", date=iso(1), start="main")
        self.repo.commit({"f.cpp": lines(1602)}, branch="b", date=iso(1), start="main")
        self.repo.git("checkout", "-q", "a")
        data, text = self.run_tool()
        f = self.file(data, "f.cpp")
        self.assertEqual(f["parallel"], [])
        self.assertEqual([e["ref"] for e in f["maybe_parallel"]], ["b"])
        self.assertIn("open PRs not observed (--no-gh)", text)
        self.assertNotIn("parallel: none\n", text)
        self.assertIn("parallel: none among observed refs (see observed)", text)

    def test_merged_closed_and_stale_work_is_not_counted(self):
        self.repo.commit({"f.cpp": lines(10), "g.cpp": lines(10), "h.cpp": lines(10)}, date=iso(9))
        m = self.repo.commit({"f.cpp": lines(11)}, branch="merged", date=iso(1), start="main")
        c = self.repo.commit({"g.cpp": lines(11)}, branch="closed", date=iso(1), start="main")
        self.repo.commit({"h.cpp": lines(11)}, branch="old", date=iso(40), start="main")
        self.repo.commit({"f.cpp": lines(12), "g.cpp": lines(12), "h.cpp": lines(12)}, branch="mine", start="main")
        data, _ = self.run_tool(prs=[pr(1, "merged", m, state="MERGED", merged=True), pr(2, "closed", c, state="CLOSED")])
        for path in ("f.cpp", "g.cpp", "h.cpp"):
            f = self.file(data, path)
            self.assertEqual((f["parallel"], f["maybe_parallel"], f["stack"]), ([], [], []), path)
        self.assertIn("1 stale", data["observed"])
        self.assertIn("1 closed", data["observed"])

    def test_squash_merged_content_is_recognised_without_gh(self):
        self.repo.commit({"f.cpp": lines(10)}, date=iso(9))
        self.repo.commit({"f.cpp": lines(10) + "new\n"}, branch="topic", date=iso(1), start="main")
        self.repo.commit({"f.cpp": lines(10) + "new\n"}, branch="main", start="main", date=iso(0.5))
        self.repo.commit({"f.cpp": lines(10) + "new\nmore\n"}, branch="mine", date=iso(0.2))
        data, _ = self.run_tool()
        self.assertIn("1 merged", data["observed"])
        self.assertEqual(self.file(data, "f.cpp")["maybe_parallel"], [])

    def test_refs_at_the_same_commit_are_one_ref(self):
        self.repo.commit({"f.cpp": lines(10)}, date=iso(9))
        sha = self.repo.commit({"f.cpp": lines(11)}, branch="a", date=iso(1), start="main")
        self.repo.git("branch", "a2", sha)
        self.repo.commit({"f.cpp": lines(12)}, branch="mine", start="main")
        data, _ = self.run_tool(prs=[pr(1, "a", sha), pr(2, "a2", sha)])
        self.assertEqual(len(self.file(data, "f.cpp")["parallel"]), 1)
        self.assertIn("refs 3", data["observed"])  # mine, and a/a2/#1/#2 as one

    def test_uncommitted_work_of_another_worktree_is_read_without_touching_it(self):
        self.repo.commit({"f.cpp": lines(10)}, date=iso(9))
        self.repo.git("branch", "other")
        other = self.tmp / "other"
        self.repo.git("worktree", "add", "-q", str(other), "other")
        (other / "f.cpp").write_text(lines(20), encoding="utf-8")
        index = Path(self.repo.git("rev-parse", "--git-dir", cwd=other)) / "index"
        index = index if index.is_absolute() else other / index
        before = index.stat().st_mtime_ns
        time.sleep(0.05)
        data, _ = self.run_tool("f.cpp", prs=[])
        self.assertEqual([(e["ref"], e["uncommitted"]) for e in self.file(data, "f.cpp")["parallel"]],
                         [("other (worktree)", True)])
        self.assertEqual(index.stat().st_mtime_ns, before)

    def test_only_source_files_count(self):
        self.repo.commit({"x.cpp": "a\n"}, date=iso(9))
        self.repo.commit({"x.cpp": "b\n", "notes.md": "n\n", "gui/server/tests/recordings/r.ts": "r\n",
                          "schema/s.json": "{}\n", "tool.py": "p\n"}, branch="mine")
        data, _ = self.run_tool()
        self.assertEqual(sorted(f["path"] for f in data["files"]), ["tool.py", "x.cpp"])
        self.assertEqual(data["excluded_files"], ["gui/server/tests/recordings/r.ts"])

    def test_a_rename_keeps_the_history_of_the_file(self):
        self.repo.commit({"old.cpp": lines(1600)}, date=iso(9))
        self.repo.commit({"old.cpp": lines(1600) + "first\n"}, branch="mine", start="main")
        self.repo.commit({"old.cpp": None, "new.cpp": lines(1600) + "first\nsecond\n"})
        data, _ = self.run_tool()
        f = self.file(data, "new.cpp")
        self.assertEqual(f["own"], [2, 0, 2])  # both commits, across the rename
        self.assertEqual(f["size_lines"], 1602)

    def test_top_limits_the_summary_and_keeps_the_log(self):
        self.repo.commit({f"f{i}.cpp": lines(1600 + i) for i in range(4)}, date=iso(9))
        for i in range(4):
            self.repo.commit({f"f{j}.cpp": lines(1700 + j + i) for j in range(4)}, date=iso(2 - i * 0.1))
        data, text = self.run_tool(*[f"f{i}.cpp" for i in range(4)], "--top", "2")
        self.assertEqual(text.count("size 17"), 2)
        self.assertIn("+2 more file(s) with a signal", text)
        self.assertEqual(len(data["files"]), 4)

    def test_a_missing_base_is_a_usage_error_and_a_run_writes_the_log(self):
        self.repo.commit({"f.cpp": lines(10)})
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(hotspots.main(["--repo", str(self.repo.root), "--base", "nope", "--no-gh"]), 2)
        self.assertIn("base ref not found", err.getvalue())
        log = self.tmp / "h.log"
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(hotspots.main(["--repo", str(self.repo.root), "--base", "main", "--no-gh", "--log", str(log), "f.cpp"]), 0)
        self.assertIn('"files"', log.read_text(encoding="utf-8"))

    def test_a_pr_whose_head_is_not_here_is_reported_as_not_observed(self):
        self.repo.commit({"f.cpp": lines(10)})
        _, text = self.run_tool("f.cpp", prs=[pr(1, "elsewhere", "0123456789abcdef0123456789abcdef01234567")])
        self.assertIn("1 PR head(s) not available locally", text)

    def test_gh_that_is_missing_fails_hangs_or_answers_garbage_is_not_observed_and_fast(self):
        self.repo.commit({"f.cpp": lines(1600)})
        cases = (("import sys; print('boom', file=sys.stderr); sys.exit(1)", "gh failed: boom"),
                 ("import time; time.sleep(20)", "gh timed out after 0.5s"),
                 ("print('{{')", "gh failed"))
        for code, expect in cases:
            started = time.monotonic()
            _, text = self.run_tool("f.cpp", "--gh-timeout", "0.5", gh_code=code)
            self.assertLess(time.monotonic() - started, 3.0, code)
            self.assertIn(expect, text)
            self.assertIn("parallel: none among observed refs", text)
        with unittest.mock.patch.object(hotspots, "GH_COMMAND", ["no-such-gh-command"]):
            parsed = hotspots.build_parser().parse_args(["--repo", str(self.repo.root), "--base", "main", "f.cpp"])
            self.assertIn("open PRs not observed (gh unavailable)", hotspots.render(hotspots.analyse(parsed))[0])

    def test_a_cut_gh_list_is_said(self):
        self.repo.commit({"f.cpp": lines(10)})
        _, text = self.run_tool("f.cpp", "--gh-limit", "0", prs=[])
        self.assertIn("PR list cut at 0", text)

    def test_directories_are_expanded_headers_count_and_gui_examples_are_left_out(self):
        self.repo.commit({"libs/a/big.hpp": lines(1600), "libs/a/note.md": "n\n", "libs/b/x.cpp": lines(3),
                          "gui/protocol/examples/e.ts": lines(3)}, date=iso(9))
        data, _ = self.run_tool("libs", "gui")
        self.assertEqual(sorted(f["path"] for f in data["files"]), ["libs/a/big.hpp", "libs/b/x.cpp"])
        self.assertEqual(self.file(data, "libs/a/big.hpp")["signals"], ["size"])
        self.assertEqual(data["excluded_files"], ["gui/protocol/examples/e.ts"])

    def test_a_path_that_is_not_in_head_is_named(self):
        self.repo.commit({"libs/a/x.cpp": lines(3)})
        data, text = self.run_tool("libs/lunch", "a/x.cpp")
        self.assertEqual(data["not_at_head"], ["libs/lunch", "a/x.cpp"])
        self.assertIn("not in HEAD (a new file, or a path that is not relative to the repository root): libs/lunch, a/x.cpp", text)

    def test_the_summary_ends_with_what_was_not_observed_and_says_none_only_among_observed_refs(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        _, text = self.run_tool("f.cpp")
        self.assertIn("attention (size with stack or parallel; a hint to look at where the change goes and who else changes the file, "
                      "not a requirement): none among observed refs (see observed)", text)
        last = text.splitlines()[-1]
        self.assertTrue(last.startswith("observed: ") and last.endswith("other clones and unpushed work are never observed"), last)

    def test_attention_does_not_change_the_exit_code_and_the_log_is_written(self):
        self.repo.commit({"f.cpp": lines(1600)}, date=iso(9))
        a = self.repo.commit({"f.cpp": lines(1601)}, branch="a", date=iso(1), start="main")
        self.repo.commit({"f.cpp": lines(1602)}, branch="b", date=iso(1), start="main")
        manifest = self.tmp / "prs.json"
        manifest.write_text(json.dumps([pr(1, "a", a)]), encoding="utf-8")
        out = io.StringIO()
        with self.fake_gh(f"import sys; sys.stdout.write(open({str(manifest)!r}).read())"), contextlib.redirect_stdout(out):
            rc = hotspots.main(["--repo", str(self.repo.root), "--base", "main", "--log", str(self.tmp / "h.log"), "f.cpp"])
        self.assertEqual(rc, 0)
        self.assertIn("attention (size with stack or parallel; a hint to look at where the change goes and who else changes the file, "
                      "not a requirement): f.cpp", out.getvalue())
        self.assertIn("PRs: 1 from gh", (self.tmp / "h.log").read_text(encoding="utf-8"))

    def test_untracked_files_of_another_worktree_count_also_in_a_new_directory_and_no_lock_is_taken(self):
        self.repo.commit({"f.cpp": lines(10)}, date=iso(9))
        self.repo.git("branch", "other")
        other = self.tmp / "other"
        self.repo.git("worktree", "add", "-q", str(other), "other")
        (other / "newdir").mkdir()
        (other / "newdir" / "new.cpp").write_text(lines(5), encoding="utf-8")  # a new file in a new directory
        real_run = subprocess.run
        statuses = []

        def recording_run(argv, *args, **kwargs):
            if "status" in argv:
                statuses.append(list(argv))
            return real_run(argv, *args, **kwargs)

        with unittest.mock.patch.object(hotspots.subprocess, "run", recording_run):
            data, _ = self.run_tool("newdir/new.cpp", prs=[])
        self.assertEqual([e["ref"] for e in self.file(data, "newdir/new.cpp")["parallel"]], ["other (worktree)"])
        self.assertTrue(statuses and all("--no-optional-locks" in argv for argv in statuses), statuses)

    def test_a_ref_counts_as_merged_only_if_all_its_files_are_in_the_base(self):
        self.repo.commit({"f.cpp": lines(10), "g.cpp": lines(10)}, date=iso(9))
        self.repo.commit({"f.cpp": lines(11), "g.cpp": lines(11)}, branch="topic", date=iso(1), start="main")
        self.repo.commit({"f.cpp": lines(11), "g.cpp": lines(12)}, branch="main", start="main", date=iso(0.5))  # g.cpp differs
        self.repo.commit({"f.cpp": lines(13)}, branch="mine", date=iso(0.2))
        data, _ = self.run_tool("f.cpp", "g.cpp")
        self.assertEqual([e["ref"] for e in self.file(data, "f.cpp")["maybe_parallel"]], ["topic"])
        self.assertIn("0 merged", data["observed"])

    def test_gh_output_of_an_unexpected_shape_is_not_observed_and_does_not_fail(self):
        self.repo.commit({"f.cpp": lines(1600)})
        for output in ('{"message": "rate limited"}', "null", '[{"number": 1}]', '["x"]'):
            _, text = self.run_tool("f.cpp", gh_code=f"print({output!r})")
            self.assertIn("open PRs not observed (gh failed: unexpected output)", text, output)

    def test_the_defaults_keep_the_run_short_and_the_thresholds(self):
        args = hotspots.build_parser().parse_args([])
        self.assertLessEqual(args.gh_timeout, 2.0)
        self.assertEqual((args.size_lines, args.churn_commits, args.churn_days), (1500, 5, 7))

    def test_split_rename(self):
        self.assertEqual(hotspots.split_rename("a/b.cpp"), (None, "a/b.cpp"))
        self.assertEqual(hotspots.split_rename("a/{b => c}/d.cpp"), ("a/b/d.cpp", "a/c/d.cpp"))
        self.assertEqual(hotspots.split_rename("a/{ => c}/d.cpp"), ("a/d.cpp", "a/c/d.cpp"))
        self.assertEqual(hotspots.split_rename("x.cpp => y.cpp"), ("x.cpp", "y.cpp"))


if __name__ == "__main__":
    unittest.main()
