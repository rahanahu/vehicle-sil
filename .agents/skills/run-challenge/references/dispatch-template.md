# Challenger dispatch

Fill this brief with concrete facts for the assigned attack surface. Omit unused
fields; when dispatching in `/swarm` (`swarm{action:"add"}`), prepend the full
instructions from `.github/agents/challenger.agent.md` so the fresh worker receives
the complete Challenger profile.

- **Subject:** repository/worktree path, exact commit (`git rev-parse HEAD`) and
  base revision (or saved diff/fingerprint for uncommitted work), and existing
  read-only build/virtualenv/node_modules paths available for immediate probing.
- **Source locations:** changed files, relevant callers/consumers, existing tests,
  and repository rules (`AGENTS.md`, relevant `docs/` pages).
- **Original requirements:** user- or consumer-observable behavior, invariants, and
  non-negotiable constraints. Distinguish these from the author's implementation
  claims.
- **Known regression (fix recheck only):** what failed previously, the verbatim
  reproducer (or line-anchored trace), and the observable outcome required on the
  fixed commit.
- **Attack surface & candidate claims:** one to three observable promises on this
  Challenger's assigned surface to try to break empirically (on a fix recheck, use
  this for any remaining promise beyond the known regression). State what callers
  or users must observe and under what conditions — including what must still work
  when invalid input is rejected or a new option is left off. Do not frame the
  chosen internal mechanism as the expected answer, and leave specific probe inputs,
  schedules, and higher-impact angles within scope open to the Challenger.
- **Reviewer-owned checks & exclusions:** test suites (`verify-gui.py`,
  `verify-web.py`, `verify.py` targets) or surfaces owned by the primary reviewer
  or a parallel Challenger so work and ports/builds do not collide.
- **Isolation & resource hygiene:** target worktree and shared build dirs are
  strictly read-only; stage all probe scripts, bundles, mutants, and redirected
  tool caches (`pytest -o cache_dir=...`) inside `mktemp -d "${TMPDIR:-/tmp}/challenger_probe.XXXXXX"`
  (`chmod 700`), capture verbatim reproducers/outputs, and remove the temp dir
  before reporting (unless an isolated `share` worktree was explicitly assigned for
  C++/sanitizer builds).
- **Deliverable:** report following `.github/agents/challenger.agent.md` with an
  explicit **`APPROVE`**, **`REJECT`**, or **`INCOMPLETE`** verdict, verbatim
  empirical commands/outputs, separated Blocking findings vs Open contract decisions
  vs Non-blocking notes, and Unchallenged areas. No product edits or recursive
  delegation.
