---
name: run-challenge
description: >-
  Coordinate an empirical-first independent Challenger and gate work on its
  verdict. Use when planning or reviewing changes to concurrency, object lifetime,
  shutdown, state machines, parsers, public contracts, test detection, CI quality
  gates, or claimed behavior-preserving performance, and before costly architecture
  commitments. Skip when the host already runs its own built-in Challenger workflow
  (such as Antigravity /teamwork). Coordinator-only; never invoke recursively.
---

# Run Challenge

Use the host's delegation and handoff mechanisms. Keep the Challenger's adversarial
methodology in `.github/agents/challenger.agent.md`; use this skill to screen,
dispatch, gate, and close the loop while preserving the project's review and CI
requirements.

## 1. Screen the change and respect host precedence

- **Defer to built-in host Challengers:** If the host orchestration environment
  already runs its own dedicated adversarial Challenger workflow and verdict
  protocol (for example, Antigravity `/teamwork` with `teamwork_challenger`),
  **do not apply this skill, `dispatch-template.md`, or `challenger.agent.md`**.
  Let the host's native Challenger workflow operate unmodified.
- **Coordinator-only:** Apply this workflow only while coordinating work in hosts
  without a built-in Challenger workflow (such as `/swarm`, standard subagent
  sessions, Codex, Copilot, Claude Code, or Gemini CLI). Implementers, reviewers,
  and Challengers return concerns to their Coordinator instead of invoking this
  skill recursively.

Inspect the requirements and actual diff. Launch an independent challenge when a
failure could affect correctness, concurrency/lifetime safety, wire or API
contracts, observability, test/gate integrity, or performance claims, or when
explicitly requested. Skip cosmetic or trivial changes without adding an approval
round.

- **Implementation challenge (default):** Freeze an identifiable commit snapshot
  and launch the Challenger(s) in parallel with the primary reviewer as soon as
  core implementation builds and passes smoke checks, without waiting for
  unrelated polish.
- **Pre-implementation design challenge (selective):** Use before committing to an
  expensive cross-layer contract or concurrency design; keep it focused on
  lock/shutdown ordering, boundary edge cases, and simpler alternatives rather
  than substituting it for post-implementation empirical probing.
- **Integration challenge:** When individual PRs or slices were already challenged,
  narrow integration checks to cross-slice glue, rebase interactions, and shared
  harnesses.

## 2. Prepare a sharp, empirical brief

Read [the dispatch template](references/dispatch-template.md) when dispatching.

Provide the original requirements, exact commit/worktree and build/virtualenv
paths (so the Challenger can execute probes immediately against real compiled
binaries, Python environments, or bundled web code without redundant setup),
candidate claims, reviewer-owned suites, and resource boundaries. Supply
repository-instruction paths (`AGENTS.md`, relevant `docs/`) because fresh workers
do not inherit them.

- **Single vs orthogonal parallel Challengers:** Launch one Challenger for a
  focused single-surface change. When a change spans distinct failure surfaces
  (for example, C++ concurrency/lifetime + Python/GUI/Web state, or schema/parser
  parity + subprocess orchestration), launch **two Challengers in parallel with
  disjoint attack surfaces** so each writes deep executable probes instead of
  skimming the whole diff.
- **Frame observable consumer promises:** State what callers or users must observe
  and under what conditions — including what must keep working when rejecting
  invalid input or leaving a new option off. Do not frame the author's chosen
  internal mechanism as the expected answer, and do not prescribe every probe;
  leave room for the Challenger to design adversarial experiments and pursue
  higher-impact failure modes within scope.
- **Resource & contention hygiene (no arbitrary time caps):** Do not impose uniform
  short time caps that encourage skipping empirical execution. Instead, state
  concrete contention rules: keep the reviewed worktree strictly read-only, stage
  all probes/caches in `/tmp` (`chmod 700`) or an isolated worktree, run at most
  one heavy C++ build at a time, and name any full suite (`verify-gui.py`,
  `verify-web.py`, `verify.py`) already owned by the primary reviewer so parallel
  workers do not collide on ports, browsers, or `scripts/busy_machine.py`. Leave
  mechanical repository-wide `rg` checklists to the primary reviewer.

## 3. Delegate independently

Launch a fresh worker with **zero inherited implementation history**:

- **Registered `challenger` subagent:** When `invoke_subagent` exposes `challenger`,
  invoke a fresh instance with the filled brief.
- **Swarm (`swarm{action:"add"}`) or role-prompt hosts:** Because Swarm workers do
  not take a subagent `TypeName`, spawn a fresh worker (`name: "challenger-..."`,
  never `fork` from the implementer) and include the complete instructions from
  `.github/agents/challenger.agent.md` followed by the task brief. Use
  `workspace: "inherit"` when probing read-only against an existing worktree/build
  from `/tmp`, or `workspace: "share"` when the Challenger needs its own worktree
  for isolated C++ mutant/ASan/TSan builds.

Follow the project's model policy (such as `.agents/rules/swarm.md` in Swarm)
according to task risk and complexity. If independent delegation is unavailable,
record the assurance gap explicitly — never label a Coordinator's or implementer's
self-check as an independent Challenger pass.

## 4. Gate on the verdict

Verify that the report pins the reviewed commit, confirms worktree cleanliness,
includes executable probe commands/outputs (unless pre-implementation or genuinely
non-executable), and separates blocking defects, open contract decisions, and
non-blocking notes:

| Verdict | Coordinator action |
| --- | --- |
| `REJECT` | Check each reproducer or line-anchored proof and its user-visible impact. Block merge/completion until established in-scope defects and contract violations are fixed and re-verified (or refuted by concrete discriminating execution evidence). Keep out-of-scope pre-existing notes separate so they do not force unplanned scope expansion. |
| `APPROVE` | Record the verified commit, empirical probes executed, and unchallenged exclusions. Combine with primary review and CI; do not extrapolate beyond the tested snapshot and scope. |
| `INCOMPLETE` | Identify the unverified claim, blocked probe, or open contract decision. Run a bounded follow-up, resolve the missing prerequisite/contract question, or escalate under project policy. Never silently treat `INCOMPLETE` as `APPROVE`. |

Assess any flagged **Open contract decision** against the original requirements and
user-facing documentation, and record the disposition even when current code
already behaves that way. Separate subject failures from broken fixtures or
parallel-workload interference. When implementer and Challenger disagree, settle
the question with a small discriminating executable probe rather than opinion or
vote counting.

If an implementation Challenger returns `APPROVE` based only on static code reading
when runnable artifacts were available, treat the check as `INCOMPLETE` and require
empirical probing of the high-risk claims.

## 5. Close the loop

1. **Fix & regression test:** Assign confirmed defects to the implementer along
   with the Challenger's minimal reproducer, and fold durable reproducers into the
   test suite where appropriate.
2. **Fix recheck:** Re-verify against the fixed commit snapshot. Supply the
   original failure reproducer/evidence and expected outcome, state any remaining
   observable promise in doubt, and leave the choice of additional adversarial
   probes open to the Challenger. Changes to touched code, callers, contracts, or
   gates invalidate prior evidence on those paths; unrelated doc/comment edits do
   not require re-running unaffected probes.
3. **Lifecycle cleanup:** You may message the same Challenger worker for an
   immediate fix-and-recheck cycle on the same unit, or spawn a fresh Challenger
   when moving to a new unit or after substantial redesign. Terminate temporary
   Challenger workers (`swarm{action:"kill"}` in Swarm) once the unit and any
   rechecks achieve `APPROVE`.
4. **Handoff record:** Record the verified commit, Challenger surfaces/claims,
   verdict, resolved findings or contract decisions, and remaining exclusions in
   the task handoff. This workflow grants no new merge, publishing, or external
   messaging authority.
