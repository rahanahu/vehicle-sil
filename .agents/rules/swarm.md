---
trigger: model_decision
description: "Apply when starting or coordinating a Swarm (/swarm) for default worker models and operating rules."
---

# Swarm models and operating rules

Unless the user specifies otherwise, set `model` explicitly in `swarm{action:"add"}`:

- **Simple / Docs / Short Lookup**: `gemini-4-hydrogen-high` (short-horizon tasks only)
- **Default (Design / Implementation / Review / Deep Trace)**: `gemini-4-neon-high`
- **High-risk / Complex**: `gemini-4-carbon-high` — only when truly needed (e.g., C++ concurrency, object lifetime, protocol boundaries, cross-repo consistency, or when `neon-high` gets stuck)

## Rules
- Keep `gemini-4-carbon-high` to at most **1 concurrent worker** unless the user asks for more.
- Escalate `hydrogen-high` to `neon-high` if a task drags on or gets stuck; escalate `neon-high` to `neon-xhigh` (localized hard bug) or `carbon-high` (broad/high-risk) via `swarm{action:"model"}`.
- Never assign two workers to edit the same file concurrently.
- Keep progress in `swarm_pane` (`plan.json`). Do not start a `monitor` or `narrator` worker.
- Use `@name` only when the named worker should wake up for an action. In summaries, quotations, or code spans, write the name without `@` unless you intend to wake that worker.
- For PR CI, run the existing `.local/tools/watch-prs.sh` from the main checkout if available, or `gh pr checks <PR> --watch`, as a background command with a completion notification. Do not chain `schedule` timers to poll CI. If the task requires a non-required job such as `linux (linux-clang-tsan)`, use a watcher that includes it or explicitly verify its result for the current PR head as described in [the risk-based verification guide](../../docs/code-map-testing.md#choose-the-verification-by-risk).
