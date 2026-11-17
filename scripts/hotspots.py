#!/usr/bin/env python3
"""Show where a change piles onto source files that are large, busy, or touched by other work. Advice only.

  scripts/hotspots.py PATH...   before the work: the files you plan to touch (HEAD is what the work builds on)
  scripts/hotspots.py           the files that the current branch, and its uncommitted work, changed

This is an optional aid for a person or an AI agent. It does not judge the design, fails nothing and asks for no
extra step: a file with "attention" is a hint to check where a new responsibility belongs (AGENTS.md, "Before you
change code"), not a request to split it.

Signals per source file (.cpp, .hpp, .py, .ts; the recordings and examples of the GUI are left out):
  size      1,500 lines or more at HEAD (--size-lines)
  churn     5 or more commits on the base in the last 7 days (--churn-commits, --churn-days)
  stack     a lower layer of the same stack (a branch that your work is built on, and is not merged) changed the file
            before you; its lines are not counted as your own change
  parallel  independent open work changed the file: another branch or pull request, or the uncommitted changes of
            another registered work tree (read with `git status`, no lock and no write)
"attention" = size with stack or parallel. Size alone, and size with churn, are shown as information.

Observed refs: the local branches, the registered work trees and, if `gh` answers within --gh-timeout seconds, the
pull requests. What it could not observe is printed on the "observed:" line, and "parallel: none" always reads "among
observed refs": other computers, other clones and unpushed work are never seen. A branch without a known pull request,
and a branch whose merge status is unknown, is listed as "unconfirmed" and not counted. Merged (also squash-merged),
closed and stale (no commit for --stale-days) refs are not counted.

Known limits: it does not see a wrong responsibility, a duplicate reading of the same data or a small file that
grows; renames and splits are followed only by git's rename detection; the dates of commits are approximate (merges
and amends change them); a branch name that was reused may be misread.

The summary goes to the output; the details (every file, as JSON) go to build/logs/hotspots.log. Exit codes: 0 always
(advice only), 2 usage error. Requires Python 3.11+ (standard library only) and git; gh is optional.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = (".cpp", ".hpp", ".py", ".ts")
EXCLUDED_PREFIXES = ("gui/server/tests/recordings/", "gui/protocol/examples/")
GH_COMMAND = ["gh"]  # the tests replace it
GH_FIELDS = "number,state,mergedAt,baseRefName,headRefName,headRefOid"
NUMSTAT = re.compile(r"^(\d+|-)\t(\d+|-)\t(.*)$")


class UsageError(Exception):
    pass


class Git:
    def __init__(self, repo: Path):
        self.common = ["git", "--no-optional-locks", "-c", "core.quotepath=false"]
        self.prefix = [*self.common, "-C", str(repo)]
        self.repo = repo

    def run(self, *args: str, stdin: str | None = None, check: bool = True) -> str:
        proc = subprocess.run([*self.prefix, *args], input=stdin, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
        if proc.returncode != 0 and check:
            raise UsageError(f"git {' '.join(args[:3])}: {proc.stderr.strip() or 'failed'}")
        return proc.stdout if proc.returncode == 0 else ""

    def ok(self, *args: str) -> bool:
        return subprocess.run([*self.prefix, *args], capture_output=True).returncode == 0

    def resolve(self, name: str) -> str | None:
        out = self.run("rev-parse", "--verify", "--quiet", name + "^{commit}", check=False).strip()
        return out or None


@dataclass
class Pr:
    number: int
    state: str  # open, merged or closed
    base: str
    head: str
    head_sha: str


@dataclass
class Worktree:
    path: str
    label: str
    dirty: set[str]


@dataclass
class Ref:
    names: list[str]
    sha: str
    date: int
    pr: Pr | None = None
    state: str = ""  # see classify()
    relation: str = ""  # self, lower, lower-diverged, upper or independent
    delta: dict[str, list[int]] = field(default_factory=dict)

    @property
    def label(self) -> str:
        label = "/".join(n for n in self.names if not n.startswith("#")) or self.names[0]
        return f"{label} (#{self.pr.number})" if self.pr and not label.startswith("#") else label


def is_source(path: str) -> bool:
    return path.endswith(SOURCE_SUFFIXES) and not path.startswith(EXCLUDED_PREFIXES)


def split_rename(text: str) -> tuple[str | None, str]:
    if " => " not in text:
        return None, text
    m = re.match(r"^(.*)\{(.*) => (.*)\}(.*)$", text)
    if m:
        pre, old, new, post = m.groups()
        return (pre + old + post).replace("//", "/"), (pre + new + post).replace("//", "/")
    old, new = text.split(" => ", 1)
    return old, new


class Observation:
    """What this computer can see now, and what it could not."""

    def __init__(self, git: Git, args: argparse.Namespace):
        self.git = git
        self.args = args
        self.now = datetime.now(timezone.utc)
        self.notes: list[str] = []  # what could not be observed
        self.pr_source = "not observed"
        self.prs: list[Pr] = []

    def load_prs(self) -> None:
        if self.args.no_gh:
            self.notes.append("open PRs not observed (--no-gh)")
            return
        try:
            out = subprocess.run([*GH_COMMAND, "pr", "list", "--state", "all", "--limit", str(self.args.gh_limit),
                                  "--json", GH_FIELDS], capture_output=True, text=True,
                                 timeout=self.args.gh_timeout, cwd=self.git.repo)
            if out.returncode != 0:
                raise RuntimeError(out.stderr.strip().splitlines()[-1] if out.stderr.strip() else "failed")
            raw = json.loads(out.stdout)
            self.pr_source = "gh"
            if len(raw) >= self.args.gh_limit:
                self.notes.append(f"PR list cut at {self.args.gh_limit}: older PRs not observed")
        except (OSError, RuntimeError, subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
            reason = ("gh unavailable" if isinstance(exc, OSError) else
                      f"gh timed out after {self.args.gh_timeout:g}s" if isinstance(exc, subprocess.TimeoutExpired)
                      else f"gh failed: {exc}")
            self.notes.append(f"open PRs not observed ({reason[:80]})")
            return
        for item in raw:
            state = "merged" if item.get("state") == "MERGED" or item.get("mergedAt") else \
                "closed" if item.get("state") == "CLOSED" else "open"
            self.prs.append(Pr(item["number"], state, item["baseRefName"], item["headRefName"], item["headRefOid"]))


def list_worktrees(git: Git) -> list[tuple[str, str, str | None]]:
    """(path, head sha, branch name or None) of the registered, existing, non-bare worktrees."""
    found, cur = [], {}
    for line in git.run("worktree", "list", "--porcelain", check=False).splitlines() + [""]:
        if not line:
            if cur.get("worktree") and cur.get("HEAD") and "bare" not in cur and "prunable" not in cur \
                    and Path(cur["worktree"]).is_dir():
                branch = cur.get("branch", "").removeprefix("refs/heads/") or None
                found.append((cur["worktree"], cur["HEAD"], branch))
            cur = {}
        else:
            key, _, value = line.partition(" ")
            cur[key] = value
    return found


def dirty_paths(git: Git, worktree: str) -> set[str]:
    out = subprocess.run([*git.common, "-C", worktree, "status", "--porcelain", "-uall"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    paths = set()
    for line in out.stdout.splitlines():
        entry = line[3:]
        paths.add(entry.split(" -> ")[-1].strip('"'))
    return paths


def numstat_delta(git: Git, tips: list[str], exclude: list[str], only: list[str] | None = None,
                  paths: list[str] | None = None) -> dict[str, list[int]]:
    """path -> [added, deleted, commits] of the commits reachable from tips and not from exclude (renames followed).
    With `only`, exactly those commits."""
    if only is not None:
        if not only:
            return {}
        out = git.run("log", "--no-walk", "--no-merges", "-M", "--numstat", "--reverse", "--format=%x01%H", *only,
                      check=False)
    else:
        out = git.run("log", "--no-merges", "-M", "--numstat", "--reverse", "--format=%x01%H", *tips,
                      "--not", *exclude, *(["--", *paths] if paths else []), check=False)
    delta: dict[str, list[int]] = {}
    touched: set[str] = set()

    def flush() -> None:
        for path in touched:
            delta[path][2] += 1
        touched.clear()

    for line in out.splitlines():
        if line.startswith("\x01"):
            flush()
            continue
        m = NUMSTAT.match(line)
        if not m:
            continue
        old, new = split_rename(m.group(3))
        entry = delta.setdefault(new, [0, 0, 0])
        if old and old in delta:
            for i, value in enumerate(delta.pop(old)):
                entry[i] += value
        entry[0] += int(m.group(1)) if m.group(1) != "-" else 0
        entry[1] += int(m.group(2)) if m.group(2) != "-" else 0
        touched.add(new)
    flush()
    return delta


def content_in_base(git: Git, base: str, sha: str) -> bool:
    """True if every file the ref changed since it forked from base has the same blob in base (applied, e.g. squash-merged)."""
    fork = git.run("merge-base", base, sha, check=False).strip()
    if not fork:
        return False
    names = git.run("diff", "--name-only", "--no-renames", fork, sha, check=False).splitlines()
    if not names:
        return False
    query = "".join(f"{base}:{n}\n{sha}:{n}\n" for n in names)
    answers = git.run("cat-file", "--batch-check", stdin=query, check=False).splitlines()
    return len(answers) == 2 * len(names) and all(answers[i].split(" ")[0] == answers[i + 1].split(" ")[0]
                                                  for i in range(0, len(answers), 2))


def line_count(data: bytes) -> int:
    return data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="scripts/hotspots.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("paths", nargs="*", help="files or directories you plan to touch (default: what the branch changed)")
    p.add_argument("--repo", default=str(ROOT), help="the work tree to look at (default: this repository)")
    p.add_argument("--base", help="base ref (default: origin/main, else main)")
    p.add_argument("--no-gh", action="store_true", help="do not call gh (pull requests are then not observed)")
    p.add_argument("--gh-limit", type=int, default=200, help="pull requests asked of gh (default: 200)")
    p.add_argument("--gh-timeout", type=float, default=2.0, help="seconds; a slower gh counts as not observed")
    p.add_argument("--size-lines", type=int, default=1500)
    p.add_argument("--churn-commits", type=int, default=5)
    p.add_argument("--churn-days", type=int, default=7)
    p.add_argument("--stale-days", type=int, default=14)
    p.add_argument("--top", type=int, default=12, help="files shown in the summary (all go to the log)")
    p.add_argument("--log", default=str(ROOT / "build" / "logs" / "hotspots.log"), help="where the details go")
    return p


def main(argv: list[str] | None = None) -> int:
    started = time.monotonic()
    args = build_parser().parse_args(argv)
    try:
        result = analyse(args)
    except UsageError as exc:
        print(f"hotspots: error: {exc}", file=sys.stderr)
        return 2
    result["seconds"] = round(time.monotonic() - started, 2)
    summary, details = render(result)
    log = Path(args.log)
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(details, encoding="utf-8")
    print(summary + f"\nlog: {log}")
    return 0


def analyse(args: argparse.Namespace) -> dict:
    git = Git(Path(args.repo).resolve())
    obs = Observation(git, args)
    base_name = args.base or ("origin/main" if git.resolve("origin/main") else "main")
    base, head = git.resolve(base_name), git.resolve("HEAD")
    if not base:
        raise UsageError(f"base ref not found: {base_name}")
    if not head:
        raise UsageError("HEAD has no commit")
    obs.load_prs()
    anc_base = set(git.run("rev-list", base).split())
    anc_head = set(git.run("rev-list", head).split())

    refs, worktrees = collect_refs(git, obs)
    own_pr = find_own_pr(git, obs, head)
    chain_lower, chain_upper = pr_chains(obs.prs, own_pr)
    started = not args.paths  # with paths, HEAD is what the work will build on, not the work: its unmerged part is a lower layer
    # A descendant of HEAD is the work's upper layer only if HEAD already holds work. Before the work starts (paths, or HEAD at
    # the base) HEAD is only a foundation, and a branch that grew from it is a sibling of the work to come: parallel.
    upper_ok = started and head != base
    if not upper_ok:
        chain_upper = set()
    classify(git, obs, refs, base, head, anc_base, anc_head, chain_lower, chain_upper, args, started, upper_ok)

    lower_anc = [r for r in refs if r.relation == "lower" and r.sha in anc_head]
    own_commits = own_commit_list(git, head, base, lower_anc,
                                  [r for r in refs if r.relation == "lower-diverged" and r.state != "in-base"]) if started else []
    own = numstat_delta(git, [], [], only=own_commits)
    current_wt = git.run("rev-parse", "--show-toplevel", check=False).strip()
    others = []
    for w in worktrees:
        if current_wt and Path(w.path).resolve() == Path(current_wt).resolve():
            for path in w.dirty:
                own.setdefault(path, [0, 0, 0])
        else:
            others.append(w)
    targets = target_files(git, args, head, own)
    # other refs are only asked about the files in question (a rename of one of them is not followed through the path filter)
    only_paths = targets["source"] if 0 < len(targets["source"]) <= 400 else None
    for r in refs:
        if r.state in ("open", "unconfirmed", "stale") and r.relation in ("lower", "lower-diverged", "independent", "upper"):
            if r.relation == "lower":
                below = [o.sha for o in lower_anc if o is not r and git.ok("merge-base", "--is-ancestor", o.sha, r.sha)]
            else:
                below = [head]
            r.delta = numstat_delta(git, [r.sha], [base, *below], paths=only_paths)

    files = [file_signals(git, obs, args, base, head, refs, others, own, path) for path in targets["source"]]
    return {
        "base": {"name": base_name, "sha": base}, "head": {"sha": head},
        "mode": "paths" if args.paths else "changes",
        "files": files, "excluded_files": targets["excluded"],
        "attention": [f["path"] for f in files if f["attention"]],
        "large_busy": [f["path"] for f in files if f["large_and_busy"]],
        "layers": layer_info(refs),
        "observed": observed_line(obs, refs, worktrees),
        "not_observed": obs.notes,
        "top": args.top, "own_files": len(own),
    }


def own_commit_list(git: Git, head: str, base: str, lower_anc: list[Ref], diverged: list[Ref]) -> list[str]:
    """The commits of the work: not in the base, not in a lower layer. A lower layer that was rewritten after the work
    was stacked on it (not an ancestor any more) is excluded by patch equivalence, which survives a rebase but not an edit."""
    if head == base:
        return []
    below = [base, *[r.sha for r in lower_anc]]
    if not diverged:
        return git.run("rev-list", "--no-merges", head, "--not", *below, check=False).split()
    keep: set[str] | None = None
    for r in diverged:
        out = set(git.run("rev-list", "--no-merges", "--cherry-pick", "--right-only", f"{r.sha}...{head}", "--not", *below,
                          check=False).split())
        keep = out if keep is None else keep & out
    return sorted(keep or [])


def collect_refs(git: Git, obs: Observation) -> tuple[list[Ref], list[Worktree]]:
    by_sha: dict[str, Ref] = {}
    no_head = 0

    def add(name: str, sha: str, pr: Pr | None = None) -> None:
        ref = by_sha.get(sha)
        if ref is None:
            date = int(git.run("log", "-1", "--format=%ct", sha).strip() or 0)
            ref = by_sha[sha] = Ref([], sha, date)
        if name not in ref.names:
            ref.names.append(name)
        ref.pr = ref.pr or pr

    by_branch = {p.head: p for p in obs.prs}
    for line in git.run("for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads").splitlines():
        name, sha = line.split(" ")
        add(name, sha, by_branch.get(name))
    for pr in obs.prs:
        if not git.ok("cat-file", "-e", pr.head_sha + "^{commit}"):
            no_head += 1
            continue
        add(f"#{pr.number}", pr.head_sha, pr)
    worktrees = []
    for path, sha, branch in list_worktrees(git):
        label = branch or f"worktree:{Path(path).name}"
        add(label, sha, by_branch.get(branch or ""))
        worktrees.append(Worktree(path, label, dirty_paths(git, path)))
    if no_head:
        obs.notes.append(f"{no_head} PR head(s) not available locally")
    return list(by_sha.values()), worktrees


def find_own_pr(git: Git, obs: Observation, head: str) -> Pr | None:
    name = git.run("symbolic-ref", "--quiet", "--short", "HEAD", check=False).strip()
    candidates = [p for p in obs.prs if p.head == name and p.state != "merged"]
    candidates += [p for p in obs.prs if p.head_sha == head]
    return candidates[0] if candidates else None


def pr_chains(prs: list[Pr], own: Pr | None) -> tuple[set[int], set[int]]:
    lower: set[int] = set()
    upper: set[int] = set()
    by_head = {p.head: p for p in prs}
    cur = own
    while cur and cur.base in by_head and by_head[cur.base].number not in lower:
        cur = by_head[cur.base]
        lower.add(cur.number)
    if own:
        frontier = {own.head}
        grew = True
        while grew:
            grew = False
            for p in prs:
                if p.base in frontier and p.number not in upper:
                    upper.add(p.number)
                    frontier.add(p.head)
                    grew = True
    return lower, upper


def classify(git, obs, refs, base, head, anc_base, anc_head, chain_lower, chain_upper, args, started, upper_ok) -> None:
    """Set state and relation. States: self, in-base, merged, closed, stale, open (open PR or uncommitted work),
    unconfirmed (no PR known: it may be abandoned or merged under another name; never counted)."""
    stale_before = int((obs.now - timedelta(days=args.stale_days)).timestamp())
    for r in refs:
        pr = r.pr
        if r.sha == head and started:
            r.state = "self"
        elif r.sha in anc_base:
            r.state = "in-base"
        elif pr and pr.state == "merged":
            r.state = "merged"
        elif pr and pr.state == "closed":
            r.state = "closed"
        elif r.date < stale_before:
            r.state = "stale"
        elif content_in_base(git, base, r.sha):
            r.state = "merged"
        elif pr:
            r.state = "open"
        else:
            r.state = "unconfirmed"
        if r.sha == head and started:
            r.relation = "self"
        elif r.sha in anc_head:
            r.relation = "lower"
        elif pr and pr.number in chain_lower:
            r.relation = "lower-diverged"
        elif pr and pr.number in chain_upper:
            r.relation = "upper"
        elif upper_ok and r.state in ("open", "unconfirmed", "stale") and git.ok("merge-base", "--is-ancestor", head, r.sha):
            r.relation = "upper"
        else:
            r.relation = "independent"


def target_files(git: Git, args, head: str, own: dict) -> dict:
    if args.paths:
        paths: list[str] = []
        for p in args.paths:
            found = git.run("ls-tree", "-r", "--name-only", head, "--", p, check=False).splitlines()
            paths += found or [p]
    else:
        paths = sorted(own)
    paths = list(dict.fromkeys(paths))
    return {"source": [p for p in paths if is_source(p)],
            "excluded": [p for p in paths if p.endswith(SOURCE_SUFFIXES) and not is_source(p)]}


def file_signals(git, obs, args, base, head, refs, worktrees, own, path) -> dict:
    blob = subprocess.run([*git.prefix, "cat-file", "blob", f"{head}:{path}"], capture_output=True)
    size = line_count(blob.stdout) if blob.returncode == 0 else 0
    since = (obs.now - timedelta(days=args.churn_days)).isoformat()
    churn = int(git.run("rev-list", "--count", f"--since={since}", base, "--", path, check=False).strip() or 0)

    def entry(r: Ref) -> dict:
        d = r.delta[path]
        return {"ref": r.label, "pr": r.pr.number if r.pr else None, "state": r.state, "added": d[0], "deleted": d[1],
                "commits": d[2], "uncommitted": False}

    stack = [entry(r) for r in refs if r.relation in ("lower", "lower-diverged") and path in r.delta
             and r.state in ("open", "unconfirmed", "stale")]
    parallel = [entry(r) for r in refs if r.relation == "independent" and path in r.delta and r.state == "open"]
    parallel += [{"ref": f"{w.label} (worktree)", "state": "open", "added": 0, "deleted": 0, "commits": 0,
                  "uncommitted": True} for w in worktrees if path in w.dirty]
    maybe = [entry(r) for r in refs if r.relation == "independent" and path in r.delta and r.state == "unconfirmed"]
    upper = [entry(r) for r in refs if r.relation == "upper" and path in r.delta]
    sig = {"size": size >= args.size_lines, "churn": churn >= args.churn_commits, "stack": bool(stack),
           "parallel": bool(parallel)}
    return {"path": path, "size_lines": size, "churn_commits": churn, "own": own.get(path),
            "stack": stack, "parallel": parallel, "maybe_parallel": maybe, "upper": upper,
            "signals": [k for k, v in sig.items() if v], "attention": sig["size"] and (sig["stack"] or sig["parallel"]),
            "large_and_busy": sig["size"] and sig["churn"] and not (sig["stack"] or sig["parallel"])}


def layer_info(refs: list[Ref]) -> dict:
    def names(rs):
        return [f"{r.label} [{r.state}]" for r in rs]

    return {"lower": names([r for r in refs if r.relation in ("lower", "lower-diverged") and r.state != "in-base"]),
            "upper": names([r for r in refs if r.relation == "upper"])}


def observed_line(obs: Observation, refs: list[Ref], worktrees: list[Worktree]) -> str:
    counts = {s: sum(1 for r in refs if r.state == s) for s in ("open", "merged", "closed", "stale", "unconfirmed", "in-base")}
    parts = [f"refs {len(refs)} ({counts['open']} open, {counts['merged']} merged, {counts['in-base']} in base, "
             f"{counts['closed']} closed, {counts['stale']} stale, {counts['unconfirmed']} unconfirmed)",
             f"worktrees {len(worktrees)} (uncommitted work found in {sum(1 for w in worktrees if w.dirty)})",
             f"PRs: {len(obs.prs)} from {obs.pr_source}" if obs.prs else "PRs: " + ("not observed" if obs.notes else "none found")]
    return ", ".join(parts)


def fmt_entry(e: dict) -> str:
    lines = "uncommitted" if e["uncommitted"] else f"+{e['added']}/-{e['deleted']}"
    return f"{e['ref']} {lines}" + ("" if e["state"] in ("open", "unconfirmed") else f" [{e['state']}]")


def file_line(f: dict, qualifier: str) -> str:
    parts = [f"size {f['size_lines']}", f"churn {f['churn_commits']}"]
    if f["stack"]:
        added = sum(e["added"] for e in f["stack"])
        deleted = sum(e["deleted"] for e in f["stack"])
        parts.append(f"stack {len(f['stack'])} lower layer(s) +{added}/-{deleted}: " + ", ".join(fmt_entry(e) for e in f["stack"]))
    else:
        parts.append("stack none")
    if f["parallel"]:
        parts.append("parallel: " + ", ".join(fmt_entry(e) for e in f["parallel"]))
    else:
        parts.append("parallel: none among observed refs" + (f" ({qualifier})" if qualifier else ""))
    if f["maybe_parallel"]:
        shown = ", ".join(fmt_entry(e) for e in f["maybe_parallel"][:3])
        more = len(f["maybe_parallel"]) - 3
        parts.append(f"unconfirmed, not counted ({len(f['maybe_parallel'])}): {shown}" + (f", +{more} more" if more > 0 else ""))
    return f"  {f['path']}: " + " | ".join(parts)


def render(result: dict) -> tuple[str, str]:
    unknown = [e for f in result["files"] for e in f["maybe_parallel"]]
    qualifier = "see observed" if result["not_observed"] or unknown else ""
    lines = [f"hotspots (advice only): base {result['base']['name']}@{result['base']['sha'][:8]}, head HEAD@{result['head']['sha'][:8]}"]
    sig = [f for f in result["files"] if f["signals"]]
    lines.append(f"files: {len(result['files'])} source file(s) checked, {len(sig)} with a signal"
                 + (f", {len(result['excluded_files'])} excluded by path" if result["excluded_files"] else ""))
    ranked = sorted(sig, key=lambda f: (not f["attention"], -f["size_lines"]))
    lines += [file_line(f, qualifier) for f in ranked[:result["top"]]]
    if len(ranked) > result["top"]:
        lines.append(f"  +{len(ranked) - result['top']} more file(s) with a signal (see the log)")
    quiet = len(result["files"]) - len(sig)
    if quiet:
        lines.append(f"  {quiet} file(s) with no signal among observed refs")
    lines.append("attention (size with stack or parallel; a hint to check placement, not a requirement): "
                 + (", ".join(result["attention"]) or "none"))
    if result["large_busy"]:
        lines.append("large and busy, information only: " + ", ".join(result["large_busy"]))
    layers = result["layers"]
    if layers["lower"]:
        lines.append("lower layers: " + ", ".join(layers["lower"]))
    if layers["upper"]:
        lines.append("upper layers: " + ", ".join(layers["upper"]))
    lines.append("observed: " + result["observed"] + ("; not observed: " + "; ".join(result["not_observed"]) if result["not_observed"] else "")
                 + "; other clones and unpushed work are never observed")
    summary = "\n".join(lines)
    details = summary + "\n\n" + json.dumps(result, indent=1, ensure_ascii=False) + "\n"
    return summary, details


if __name__ == "__main__":
    sys.exit(main())
