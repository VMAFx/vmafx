<!-- markdownlint-disable MD060 -->
# Research-0091: openai/symphony SPEC review — adoptable shapes for the fork

- **Date**: 2026-05-09
- **Author**: Claude (Opus 4.7), under @Lusoris direction
- **Drives**: [ADR-0355](../adr/0355-symphony-agent-dispatch-infra.md)
- **Status**: Final

## Source under review

- [openai/symphony — SPEC.md](https://github.com/openai/symphony/blob/main/SPEC.md)
  (read 2026-05-09).

Symphony is OpenAI's experimental agent-orchestration runtime: an
Elixir/OTP daemon that pairs an `Issue` tracker (Linear) with a
`Codex` execution sandbox and a `Workflow` brief, with explicit
*Reconciliation* semantics for in-flight runs.

## What Symphony does well — and what we're stealing

| Symphony primitive | Symphony §ref | What we adopt | What we drop |
|---|---|---|---|
| `Tracker` issue normalisation | §3.1 | Read-only `BacklogTracker` class, typed `BacklogItem` dataclass, parses `.workingdir2/BACKLOG.md`. | Symphony's write-side (`Tracker.update_status`) — BACKLOG.md edits stay manual per the fork's `read AND update` rule. Linear backend dropped — we're file-based. |
| `Reconciliation` (stop in-flight runs when state changes) | §3.1 | **Pre-dispatch** version: `agent-eligibility-precheck.py` runs the three checks once, before `Agent(...)`. | The mid-flight watch loop. Our agent runs are minutes-long and fast-fail on first lint pass — pre-dispatch covers ~95 % of the wasted work. |
| `Workflow` typed YAML front matter | §4.1.2 / §4.1.3 | `.claude/workflows/_template.md` + three task-specific instances; YAML keys (`name`, `agent_type`, `isolation`, `verification`, `forbidden`, `backlog_id`) consumed by humans + the precheck. | The `phases` / `steps` DSL — Claude Code's prompt is already a free-form Markdown body, not a step graph. The body sits below the YAML and uses `{{...}}` Mustache placeholders, no execution engine. |
| `Issue` ↔ `PR` linkage | §4.1.1 | `BacklogTracker.pr_refs` field + `GitHubTracker.search_prs` / `merged_prs_since`. | None — directly portable. |
| Worktree / sandbox isolation | §4.1.4 | Already enforced by `feedback_agents_isolated_worktree_only`; the workflow template hoists the same prelude into one place. | Symphony's container/namespace shape — we're at git worktree granularity. |

## What Symphony does that we **didn't** adopt — and why

| Symphony primitive | Why dropped |
|---|---|
| **Linear tracker as source of truth.** | BACKLOG.md is the project's prioritised intent — adding Linear is a SaaS dependency, a sync surface, and a license cost for a 2-contributor fork. The `BacklogTracker` abstraction means we *could* swap to Linear later with one file changed; today the cost is unjustified. |
| **Codex daemon.** | Claude Code's existing harness already does sandboxed agent execution + tool-use + commit. Codex is a parallel runtime with no benefit for users who already pay for Claude Pro. |
| **Elixir/OTP runtime.** | Two-contributor fork; nobody on the team writes Elixir. Adding the BEAM as a build dependency is a multi-week investment with no leverage. |
| **`Workflow.steps` DSL.** | The fork's prompt-engineering pattern is "free-form Markdown body + a few placeholder slots". Encoding a step graph in YAML would force every brief to be re-written into a constrained DSL — strictly worse than the current Markdown brief shape, with no observability benefit until we have a runtime to consume the graph. |
| **`Issue.assignee` / `Issue.labels` model.** | BACKLOG.md rows have no assignee field — the fork's contributor count is two. Labels live in tier prefixes (`T0…T7`) and ad-hoc bold tags inside the row. We expose tier as `BacklogItem.priority`, which is enough. |
| **Mid-flight `Reconciliation` watch loop.** | Pre-dispatch version costs ~30 lines of Python; mid-flight needs a long-lived process polling the tracker, which is the wrong shape for a CLI agent harness. Re-evaluate if we ever observe an agent finishing its lint pass while the same item ships on master. |
| **`Workflow.notification` channels (Slack / email).** | Claude Code already surfaces dispatch + completion in the user's terminal. Adding webhooks for a 2-person team is over-engineered. |
| **Symphony's audit log.** | Git history + `docs/state.md` already serve this purpose. |

So: 5 of 12 primitives adopted in shape, 7 deliberately dropped. The
adopted set is exactly the "stop briefing in prose, stop dispatching
NO-OPs" surface.

## Cost / benefit summary

| Cost | Benefit |
|---|---|
| 1 PR. ~700 LoC in Python + Markdown. Stdlib-only — no new runtime, no new SaaS, no new pip dependency. | Closes the two confirmed NO-OP dispatch failure modes from this session (`vmaf_tiny_v3` registry / T7-5 NOLINT sweep — together ~60 KB context burned). Provides a typed brief shape so future briefs accrete instead of re-inventing. Provides a tracker abstraction reusable by any future status-audit or state-reporter script. |

The break-even point against status-quo is one prevented NO-OP
dispatch. We had two this session.

## Files added

- `.claude/workflows/_template.md`
- `.claude/workflows/codeql-alert-sweep.md`
- `.claude/workflows/simd-port.md`
- `.claude/workflows/feature-extractor-port.md`
- `scripts/lib/__init__.py`
- `scripts/lib/backlog_tracker.py`
- `scripts/ci/agent-eligibility-precheck.py`
- `docs/adr/0355-symphony-agent-dispatch-infra.md`
- `docs/development/agent-dispatch.md`
- `docs/research/0091-symphony-spec-review.md` (this file)

## Smoke results (2026-05-09)

`BacklogTracker` parses 101 rows from the live `.workingdir2/BACKLOG.md`
(17 OPEN, 78 closed-class, the rest BLOCKED/DEFERRED/IN_FLIGHT). Spot
checks against the user-cited examples:

```text
T0-1   → status=DONE     priority=0  prs=[#72]      (ground truth: DONE — PR #72)
T0-3   → status=REMOVED  priority=0  prs=[#62]      (ground truth: REMOVED 2026-04-25)
T1-1   → status=DONE     priority=1  prs=[#91]      (ground truth: DONE — PR #91)
T7-5   → status=DONE     priority=7  prs=[#82]      (ground truth: DONE — PR #82, closeout PR #388)
T3-7   → status=OPEN     priority=3  prs=[]         (ground truth: open, deprioritised)
TA-VOCAB → status=OPEN   priority=None prs=[#394, #401, #428] (ground truth: cross-stream addendum)
```

`agent-eligibility-precheck.py` smoke against the same set:

```text
T0-1   → exit=1, ::error agent-eligibility: T0-1 is DONE
T7-5   → exit=1, ::error agent-eligibility: T7-5 is DONE
T3-7   → exit=0, VERDICT=PASS — dispatch eligible
```

## Open questions / future work

- **Mid-flight reconciliation.** Skipped today (see table). Revisit
  if we ever ship a same-scope item *during* an agent run.
- **Harness `Agent.preDispatch` hook.** When the Claude Code
  `settings.json` schema gains it, wire the precheck in
  unconditionally so opt-in becomes opt-out.
- **`vulkan-port.md` workflow instance.** Add when
  T-VK-VIF-1.4-RESIDUAL closes — Vulkan port has enough recurring
  shape (image-import patches, NVIDIA-driver dispatch overhead) to
  justify a fourth template.

## References

- Symphony SPEC §3.1, §4.1.1, §4.1.2, §4.1.3.
- ADR-0355 (this digest's parent decision).
- CLAUDE.md `feedback_agents_isolated_worktree_only`,
  `feedback_verify_state_before_dispatch`,
  `feedback_deliverables_checklist_strict_parser`.
