---
name: run-challenge
description: >-
  Coordinate a focused independent Challenger and handle its findings. Use when
  planning or reviewing work that changes concurrency, object lifetime, shutdown,
  public contracts, test detection, CI quality gates, or claimed behavior-preserving
  performance, and before costly architecture commitments. Also use for an explicit
  request to challenge assumptions or seek counterexamples. This is a Coordinator
  workflow, not a worker's self-review or a recursive challenger launcher.
---

# Run Challenge

Use the host's existing delegation and handoff mechanisms. Keep the Challenger's
reasoning instructions in its agent profile; use this skill to select, dispatch,
and close the work. Preserve the project's existing review and CI requirements.

## 1. Screen the change

Apply this workflow only while coordinating the work. An implementer, reviewer,
or Challenger should return relevant concerns to its Coordinator instead of
launching this workflow recursively.

Inspect the actual requirements and change. Identify important claims whose
failure would affect correctness, observability, reliability, or maintenance.
Use an independent challenge when a material claim or uncertainty warrants it,
or when explicitly requested. Skip cosmetic and otherwise low-risk changes
without creating a new approval step; record a short reason in existing task
notes when useful.

For an expensive design decision, challenge the requirements and tradeoffs before
implementation. For implementation verification, freeze an identifiable snapshot
and launch the Challenger in parallel with the primary reviewer as soon as core
implementation is ready, without waiting for unrelated follow-up test or doc
commits. For an integration branch whose individual steps were already challenged,
narrow the scope to glue code, integration wiring, and test harnesses added or
changed during integration. Do not automatically require both phases for every
change.

## 2. Prepare a bounded brief

Read [the dispatch template](references/dispatch-template.md) only when dispatching.

Provide the original requirements, exact subject and evidence locations,
candidate claims, existing verification, and a concrete budget. Supply applicable
repository-instruction paths because a worker may not inherit them. Share factual
test results and reproduction information; do not frame the task as confirming
someone else's approval or finding a predetermined number of defects.

By default, start one Challenger per substantive change unit with a focused
assignment covering one to three important claims; multiple Challengers are
allowed when distinct independent perspectives are useful. If no project budget
exists, use a soft 15-minute initial budget and at most one heavy build/test
process at a time. Apply any stricter project resource limits. A soft budget means
report and return at the next safe stopping point, not abandon processes or delete
evidence.

Frame each claim so the Challenger can tell which observed outcomes would
violate it. If the change rejects an invalid input or blocks an unsafe operation,
include what callers must still be able to do. Name concrete conditions when
the contract depends on them. Let the Challenger choose where and how to try to
break the claim, or pursue a more consequential claim within the requirements.

Allocate an isolated workspace and evidence destination. Keep heavy probes out
of shared source/build directories. Reuse existing verification where it answers
the question, and identify the primary reviewer's work to avoid needless repeats.
Leave mechanical removal checklists (such as repository-wide `rg` sweeps) to the
primary reviewer so the Challenger stays focused on adversarial counterexamples
for concurrency, lifetime, boundaries, and contracts.

## 3. Delegate independently

Resolve the installed `challenger` agent and launch a fresh independent worker
without inherited implementation history for each substantive change unit, using
the current host's documented delegation mechanism (in a Swarm, spawn a new worker
via `add` rather than forking implementation context).

If the host accepts role prompts rather than registered agent names, read the
installed `challenger.agent.md` profile and pass its complete instructions to a
fresh worker along with the brief. Record which mechanism was used. Do not invent
Swarm commands, tool names, or supported configuration fields.

Select the model according to the difficulty of the change and the project's
model policy; do not choose a higher-tier model solely because the role is
Challenger or Reviewer. Give the worker the context it needs without inheriting
the implementation conversation.

If independent delegation or the agent profile is unavailable, report the
integration gap and affected assurance to the Coordinator's task record. Do not
label a Coordinator's self-review as an independent challenge or mark the check
complete. Continue other authorized work while applying existing acceptance policy.

## 4. Assess the report

Confirm that the report identifies its snapshot, evidence, established findings,
and unchallenged areas. Assess any reported mismatch that needs a contract decision
against the original requirements and user-facing documentation. Record the
disposition even when the behavior matches existing code. Treat the result as
input to the existing decision process:

| Result | Coordinator action |
| --- | --- |
| `FINDINGS` | Check the requirement, evidence, consequence, and severity. Send established issues to the implementer with the reproducer or reasoning. Apply existing acceptance policy; a minor finding does not automatically block all work. |
| `NO_FINDINGS_IN_CHECKED_SCOPE` | Record the covered scope and limits. Combine this evidence with the primary review and CI. Do not generalize it into a proof of safety. |
| `INCOMPLETE` | Identify the missing evidence and its materiality. Choose a bounded follow-up, an explicit risk disposition allowed by existing policy, or the required escalation. Do not silently convert it to a pass. |

For every result, handle material unresolved gaps using the `INCOMPLETE` procedure.
Fixing an established issue does not resolve a separate unverified requirement.

Separate a subject failure from a broken fixture, missing dependency, or resource
interference. A concrete, complete code-path argument under stated assumptions can
establish a finding without executing a reproducer; a vague possibility is an
unresolved hypothesis, not an established defect.

For disagreement, seek the smallest discriminating evidence within a stated
budget. Do not settle by counting approvals, invoking endless reviewers, or
automatically accepting either the implementer's or Challenger's verdict.

## 5. Close the loop

Assign product fixes and durable regression tests to the implementer. Preserve a
useful reproducer before disposing of scratch resources.

For a fix recheck, ask the Challenger to verify the known failure against the
fixed snapshot. Supply the reproducer or code-path evidence and the expected
outcome. If a material question remains beyond that regression, state the
observable promise in doubt and leave the choice of new cases to the Challenger.

Compare the final snapshot with the reviewed one: changes to the relevant code, callers,
contracts, or test/gate behavior invalidate the affected evidence; unrelated
changes do not require restarting the entire review. You may reuse the same
Challenger for a prompt fix-and-recheck cycle on the same change unit; prefer
starting a fresh Challenger after a long idle wait or when moving to a different
change unit (strict cache-TTL tracking or fixed-interval rotation is unnecessary).

Extend the budget or launch an additional Challenger when a distinct independent
perspective or separate material question warrants it. Stop once the evidence is
sufficient under existing policy, and terminate the Challenger worker (e.g.,
`kill` in a Swarm) once verification and any needed recheck for its change unit
are complete.

Record the snapshot, hypotheses checked, result, findings/disposition, important
gaps, and evidence locations in the existing handoff. State the actual challenge
status when declaring the parent task complete. This workflow grants no new merge,
publishing, or external messaging authority.
