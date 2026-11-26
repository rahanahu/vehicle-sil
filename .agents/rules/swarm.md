---
trigger: model_decision
description: "Apply when starting a Swarm (/swarm) to assign default worker roles and models unless the user specifies otherwise."
---

# Default Swarm Models

Unless the user specifies otherwise, set `model` explicitly in `swarm{action:"add"}`:

- **Narrator**: `gemini-4-hydrogen-low` (do not spawn `monitor`)
- **Simple / Docs / Short Lookup**: `gemini-4-hydrogen-high` (short-horizon tasks only)
- **Default (Design / Implementation / Review / Deep Trace)**: `gemini-4-neon-high`
- **High-risk / Complex**: `gemini-4-carbon-high` — only when truly needed (e.g., C++ concurrency, object lifetime, protocol boundaries, cross-repo consistency, or when `neon-high` gets stuck)

## Rules
- Keep `gemini-4-carbon-high` to at most **1 concurrent worker** unless the user asks for more.
- Escalate `hydrogen-high` to `neon-high` if a task drags on or gets stuck; escalate `neon-high` to `neon-xhigh` (localized hard bug) or `carbon-high` (broad/high-risk) via `swarm{action:"model"}`.
- Never assign two workers to edit the same file concurrently.
