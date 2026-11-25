---
trigger: model_decision
description: "Apply when starting a Swarm (/swarm) to assign default worker roles and models unless the user specifies otherwise."
---

# Default Swarm Models

Unless the user specifies otherwise, set `model` explicitly in `swarm{action:"add"}`:

- **Narrator**: `gemini-4-hydrogen-low` (do not spawn `monitor`)
- **Simple / Docs / Lookup**: `gemini-4-hydrogen-high`
- **Default (Design / Implementation / Review)**: `gemini-4-neon-high`
- **High-risk / Complex**: `gemini-4-carbon-high` — only when truly needed (e.g., C++ concurrency, object lifetime, protocol boundaries, or when `neon-high` gets stuck)

## Rules
- Keep `gemini-4-carbon-high` to at most **1 worker per Swarm** unless the user asks for more.
- Escalate `hydrogen-high` to `neon-high` (or `neon-high` to `carbon-high`) via `swarm{action:"model"}` if a worker gets stuck.
- Never assign two workers to edit the same file concurrently.
