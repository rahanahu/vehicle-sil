---
name: agent-prompt-design
description: >-
  Write or revise assignments for coding, research, measurement, and review agents.
  Specify the outcome, constraints, and delegated authority so the agent can choose how to do the work.
  Use for Swarm briefs, goals for leads or coordinators, subagent tasks, handoffs, instructions for a running session,
  task rules in CLAUDE.md or AGENTS.md, and reusable delegation templates. Requests include
  指示書, 依頼文, 引き継ぎメモ, 差し込む文面, ゴールを書いて, and エージェントに渡す/やらせたい.
  Also use to revise instructions that over-constrain an agent or trigger routine approval requests.
  Exclude general prompt-engineering advice, chatbot system prompts, image prompts, human task lists,
  and agent framework code.
---

# Development Agent Prompt Design

## Core principles

Design prompts to achieve outcomes, not merely to make agents execute a checklist.
A checklist-only prompt fails as soon as reality diverges from the plan; an agent that understands the objective and its boundaries can adapt.

Separate what the **owner** must establish (product requirements, constraints, authority boundaries) from what the assigned agent should decide (design and execution choices).
"Owner" means whoever assigns the work and holds authority over it: usually the user, or a coordinator agent acting for the user.

Do not relax safety, quality assurance, or explicit constraints in the name of autonomy.
Autonomy is only useful if the owner can trust that the boundaries hold.

## 1. Identify the role and assignment type

| Role | Focus of instructions |
|---|---|
| Coordinator or development lead | Domain objectives, requirements, discretion, cross-team boundaries, completion criteria |
| Implementation agent | Purpose of the change, scope, contracts to preserve, verification criteria |
| Research or measurement agent | Questions to resolve, evidence quality, comparison conditions, deliverables |
| Independent reviewer | Review subject, evaluation criteria, evidence for findings, independence |

Distinguish design-only work, research or measurement only, isolated prototyping, and production implementation according to what is authorized.
A coordinator may also implement, but is responsible for the domain outcome, not only individual tasks.
When promoting a subagent to coordinator, state explicitly that the new responsibilities and delegated authority supersede its previous limited role; otherwise the agent keeps obeying its old, narrower instructions. Preserve higher-priority safety rules.

## 2. Classify the instructions

Separate these elements from the original request and existing plans:

- Objective: why the work matters, what value it provides, and to whom.
- Requirements: required performance, functionality, quality, and invariants.
- Mandatory constraints: permissions, safety, compatibility, protection of other agents' changes, measurement comparability, and similar boundaries.
- Initial approach: current design proposals, measurement procedures, PR sequencing, implementation candidates.
- Delegated decisions: choices the agent can make or revise based on evidence.
- Completion criteria: observable outcomes and evidence that the objective has been achieved.

Do not downgrade explicit requirements to suggestions. If it is unclear whether an instruction is mandatory or merely an initial approach, keep it as a requirement and note where discretion is possible — wrongly loosening a requirement is harder to undo than wrongly keeping one.
When the owner approves findings from a review, keep each approved outcome in scope. A suggested fix (such as splitting a module) becomes mandatory only if the owner or a binding constraint requires it. Let the agent choose a smaller fix that meets the requirement, or cite a test or other evidence if the current implementation already meets it.
Do not invent authorization for prototyping, additional expenditure, production integration, or changes to another agent's work. If you propose a change in authority, disclose it to the user.

### When information is missing

Ask the user only when the gap is about authority or a mandatory constraint (e.g. whether production changes are allowed, whether a hold may be lifted) and a wrong guess would be costly.
For anything else, draft the prompt with the most reasonable assumption and list the assumptions after the prompt so the user can correct them.

## 3. Grant coordinators appropriate discretion

Within approved objectives and authority boundaries, normally delegate:

- Identifying needed investigations, testing hypotheses, and follow-up research.
- Planning, sequencing, splitting PRs, selecting models, and delegating work.
- Communicating with other agents and coordinating responsibilities, interfaces, and dependencies.
- Technical decisions, verification, arranging independent reviews, and addressing findings.
- Updating plans, decision records, and progress records within their own domain.
- Implementing, testing, checking CI, and merging when authorized.

Do not require approval merely because an investigation was not in the original plan: the plan was written with less information than the agent now has.
Distinguish ownership of a responsibility from who actually implements it.
Do not route routine cross-team coordination through the owner; the owner becomes a bottleneck and work stalls while waiting.
When prerequisites for approved work are satisfied, instruct the agent to start without asking again.
Do not impose uniform caps on session lifetime or planner steps for coordinators, implementers, or reviewers; split or hand off work along natural scope boundaries instead.
Do not pin or select a higher-tier model solely because of a role title; choose models based on available models and task difficulty.
Do not authorize the agent to lift explicit holds or prohibitions on its own.

## 4. Define stopping and escalation conditions

Distinguish progress reporting from approval requests. Continue actionable work after reporting.
Do not substitute proposal cards or option lists for actual investigation, coordination, or execution.
When ordinary technical judgment yields a reasonable choice, instruct the agent to act within its authority and record its rationale when useful.

Reserve owner decisions for major product-policy changes, material scope expansion, irreversible actions, insufficient permissions or budget, and significant conflicts between agents that cannot be resolved safely.
Never bypass explicit approval requirements or tool permission checks.
An escalation states the decision needed, why it is outside delegated authority, the evidence, the recommendation, and the impact. Continue unaffected work meanwhile.

If a genuine prerequisite is missing, record the blocker, the resume condition, and how notification or handoff happens. Resume without renewed approval once the prerequisite is met, if the environment permits.
For Swarm PR CI, follow the [Swarm operating rules](../../rules/swarm.md) for background watching. For other builds or waits, prefer reactive completion notifications. A one-shot timer is suitable for a specific deadline, not for repeated CI status checks.
Do not assume a terminated session restarts automatically. If the environment lacks notification or resumption mechanisms, leave enough information for a future session to resume.
End when the work is complete or no useful actionable work remains. Do not manufacture tasks.

## 5. Assemble the prompt

Use the list below as a drafting aid. A paragraph is enough when it gives the recipient what they need to act.
Carry forward earlier decisions and the reasons that affect the next choice. Point to the relevant section of an approved issue, review, or specification and to existing rules the recipient can read. If a source is inaccessible, include the relevant excerpt. For a worker in an isolated git worktree on the same machine, an absolute path in the main worktree can make untracked local material available.

1. Role: assigned domain and responsibility.
2. Goal: a concise statement of the desired outcome.
3. Context and requirements: product requirements, targets, relevant references.
4. Discretion: decisions the agent may make; investigation, delegation, and coordination it may conduct.
5. Constraints: prohibitions, authorization boundaries, safety, quality, reproducibility.
6. Completion criteria: the evidence of success, any required records or reviews, and the authorized endpoint (for example, verified local changes, an open PR, or a merge).
7. Escalation: decisions reserved for the owner; how to handle waiting and continued work.

Language: write the prompt in the language the user asks for; otherwise match the language of the existing prompts or of the user's request.

### Output format

- New prompt: a single copy-ready block, then (if any) a short list of assumptions.
- Supplementary instructions for an existing session: only what changes or needs clarification, with an explicit sentence preserving existing constraints.
- Revision of an existing prompt: the revised prompt, then a change list. Mark every change that affects authority or constraints (added, removed, or loosened) so the user can approve it consciously.

### Example

Before (over-prescriptive):

```
1. Measure latency for configs A, B, C. 2. Report results and wait for approval.
3. If approved, measure D. 4. Do not change any code.
```

After:

```
Role: Measurement agent for the DDS transport latency study.
Goal: Determine whether config B meets the p99 < 2 ms target and what dominates latency if not.
Requirements: Use the existing baseline (config A, commit abc123) and identical load profiles so results are comparable.
Discretion: Choose measurement order, add configurations or repeat runs when needed to support a conclusion, and test hypotheses about bottlenecks. No approval needed for additional measurements.
Constraints: Do not modify production code. Isolated prototypes are not authorized; if one seems necessary, propose it with evidence and continue the remaining measurements. Do not stop other agents' workloads; if interference is suspected, record it and coordinate timing with the affected agent.
Completion: A short report stating target attainment, dominant bottleneck, expected improvement and cost of the options, and reproduction steps. Raw data saved under results/.
Escalation: Only for prototype authorization or if the baseline itself appears invalid. Report progress without waiting.
```

## 6. Research and performance-measurement prompts

Require conclusions that support technical decisions, not just tables of measurements.
Where relevant, ask for target attainment, dominant bottlenecks, regressions, correctness, reproducibility, expected improvement, and cost.
Preserve mandatory measurement conditions and baselines; delegate additional measurements and execution order.
If reducing the measurement matrix is authorized, require that conclusions remain supported and omissions be justified.
Avoid measurement interference from parallel workloads without unilaterally stopping other agents.
Make authorization for isolated prototyping explicit. Permission to prototype does not imply permission to integrate into the product.

## 7. Review the prompt before returning it

- Does the role have appropriate responsibility and discretion?
- Are requirements separated from procedures, and mandatory constraints from initial approaches?
- Are all explicit constraints and authorization boundaries preserved?
- Does routine investigation, task initiation, sequencing, or coordination require unnecessary approval?
- Are implementation approaches or model choices unnecessarily fixed?
- Do completion criteria measure achievement of the objective rather than checklist consumption?
- Are other agents' responsibilities, work, shared resources, and QA protected?
- Are waiting, completion, and resumption conditions realistic for the environment?
- Does the prompt avoid unnecessary rules, documents, or management mechanisms?

Fix any deficiency before returning. When revising, disclose authority or constraint changes as described in §5.

## 8. Evaluation examples

- Suspected message loss at startup: if investigation is within the coordinator's authority, start or delegate it instead of stopping at an approval card.
- Documentation edits conflict with another stream's PR: coordinate sequencing directly, record dependencies, and wait if necessary. Do not ask the owner to choose the sequence.
- Measurement-only assignment: delegate measurement order, hypothesis testing, and follow-ups, but do not authorize production code changes. State explicitly whether isolated prototypes are permitted.
- Explicitly deferred changes to the standard GUI: do not lift the hold in the name of autonomy. Escalate if lifting it is necessary and continue unrelated work.

### Swarm brief for an approved release review

> Address the findings under `In Scope` in the attached release review so existing clients can upgrade without changing their integrations. Preserve the public CLI and wire format. Choose how to group the fixes and update affected callers, tests, or docs as needed.
>
> For the reconnect finding, show that an existing client can reconnect to the same run and read its current state. Cite the check for any finding already resolved on main. Open a PR for any changes and include the verification results.
