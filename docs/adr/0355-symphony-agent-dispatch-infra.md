<!-- markdownlint-disable MD060 -->
# ADR-0355: Symphony-inspired agent-dispatch infrastructure

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: @Lusoris, Claude (Opus 4.7)
- **Tags**: `agents`, `ci`, `tooling`, `fork-local`

## Context

This session repeatedly burned tokens on three failure modes:

1. **Free-prose agent briefs.** Every dispatch wrote a fresh prompt
   from scratch — missing constraints, missing reproducer commands,
   inconsistent worktree-isolation preludes. Each brief was a
   one-off; nothing accreted into reusable templates.
2. **Tracker-state drift.** The user's prioritised intent lives in
   `.workingdir2/BACKLOG.md`, but every agent rebuilt its mental
   model by re-grepping master. Twice this session we dispatched
   work for items that BACKLOG.md (and merged PRs) already showed
   as DONE — `vmaf_tiny_v3` registry promotion (closed by PR #351)
   and the T7-5 NOLINT sweep (closed by PR #327 + PR #388).
3. **No reconciliation gate.** Symphony §3.1 stops in-flight runs
   when issue state changes; we had nothing equivalent. The cost
   of a NO-OP agent run is roughly 30 KB of context plus 5–10
   minutes of wall-clock — well above the cost of a 50-line
   precheck.

OpenAI's Symphony spec
([openai/symphony §3.1, §4.1.1, §4.1.2, §4.1.3](https://github.com/openai/symphony/blob/main/SPEC.md))
addresses all three with named primitives: a normalised `Issue`
model behind a `Tracker`, a typed YAML front matter on every
workflow brief, and a *Reconciliation* hook that refuses ineligible
dispatches. Adopting Symphony wholesale (Elixir runtime,
Codex daemon, Linear-only tracker) is too expensive for a fork that
already has Claude Code's harness and BACKLOG.md as the prioritised
queue. Adopting only the **shapes** of those primitives is cheap.

## Decision

Land three thin, in-repo artefacts that mirror the Symphony shapes
without buying the runtime:

1. `.claude/workflows/` directory with a typed-YAML-front-matter
   `_template.md` plus three task-specific instances
   (`codeql-alert-sweep.md`, `simd-port.md`,
   `feature-extractor-port.md`). Front matter is consumed by humans
   today and by the precheck script (`backlog_id` field) on every
   run; it can be parsed by future tooling without any further
   schema work.
2. `scripts/lib/backlog_tracker.py` — read-only Python module that
   parses BACKLOG.md into typed `BacklogItem` rows and wraps `gh`
   PR queries (`GitHubTracker.merged_prs_since`,
   `open_agent_branches`, `search_prs`). One module, two classes,
   exhaustively unit-testable against the real BACKLOG.md.
3. `scripts/ci/agent-eligibility-precheck.py` — pre-dispatch gate
   that runs three checks (BACKLOG row not closed; no merged PR
   already mentions the scope; no in-flight harness task or open
   PR branch on the same scope). Exits 0/1; verdicts on stderr in
   GitHub Actions `::error` format. Documented as **MUST RUN
   BEFORE DISPATCH** at the top of `_template.md`. The Claude Code
   harness does not currently expose a pre-Agent hook, so wiring is
   manual today; if/when an `Agent.preDispatch` hook surfaces in
   `settings.json`, the script's contract is already CI-shaped and
   the wire-in is a one-line change.

The combined design is read-only, one-PR landing, and adds **zero**
runtime dependencies (stdlib only — no PyYAML, no Linear SDK).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **(a)** Adopt Symphony wholesale (Elixir runtime, Codex daemon, Linear tracker) | Battle-tested upstream design; `Reconciliation` semantics out of the box; first-class observability. | New runtime (Elixir/OTP) + new SaaS dependency (Linear); BACKLOG.md migration would need a Linear-side mirror; team is already Claude-Code-native. Multi-week investment for a fork that ships ~100 PRs/week with two contributors. | Cost out of proportion to the failure mode. We need the *shapes* (typed brief, normalised tracker, pre-dispatch hook) — not the engine. |
| **(b)** Bash + Python skill set, BACKLOG.md as truth (chosen) | Stdlib-only; one PR; reuses existing `.workingdir2/` + `gh` plumbing; precheck callable from any wrapper script today and from a future harness hook tomorrow. | Three checks aren't a true Reconciliation loop — they fire once, before dispatch, and don't watch for state changes mid-run. Manual call point until the harness exposes a pre-Agent hook. | Symphony's mid-flight `Reconciliation` is overkill when our agent runs are minutes-long and fail fast on the first lint pass. Pre-dispatch covers ~95 % of the wasted work; we add the watch loop later if data shows we need it. |
| **(c)** Status quo (free-prose briefs, manual NO-OP triage) | Zero new infrastructure; nothing to maintain. | Documented in this ADR's *Context*: 2 confirmed NO-OP dispatches this session burning ~60 KB of context; many more close calls. The cost compounds across sessions. | Already losing more time than the build-out costs. Rejected. |

## Consequences

- **Positive**:
  - One canonical brief shape per task class — copy `_template.md`
    plus the right instance, fill the `{{...}}` placeholders, ship.
  - Pre-dispatch precheck closes the NO-OP failure mode at the
    cheapest possible point.
  - `BacklogTracker` is reusable beyond the precheck — any future
    state-audit or status-reporter script imports it instead of
    re-grepping BACKLOG.md.
  - All three artefacts are stdlib-only Python and shell, so the
    fork-local lint profile (clang-tidy + cppcheck + ruff) covers
    everything without new toolchain.

- **Negative**:
  - The precheck is opt-in until the harness exposes a pre-Agent
    hook. A dispatcher who skips it gains nothing. Mitigation:
    `_template.md` opens with the precheck call as the **first**
    instruction; CLAUDE.md `feedback_verify_state_before_dispatch`
    rule already escalates this to a session-level habit.
  - BACKLOG.md row format is informal (markdown table). The parser
    is regex-based and will break if the table changes shape (e.g.
    a column is added). Tracked under the "row format reference"
    docstring at the top of `backlog_tracker.py`; any structural
    edit to BACKLOG.md should re-run the parser smoke (101 rows
    parsed, 17 OPEN, 78 closed).

- **Neutral / follow-ups**:
  - When a Claude Code `Agent.preDispatch` hook lands, wire the
    precheck via `.claude/settings.json` and remove the manual
    call from `_template.md`.
  - When the BACKLOG.md format ever migrates (Linear, JSON,
    SQLite), `BacklogTracker` is the only file that needs to
    change; everything else imports it.
  - The three workflow instances will accrete over time — the
    next addition is likely `vulkan-port.md` once T-VK-VIF-1.4-RESIDUAL
    closes.

## References

- Source spec: [openai/symphony §3.1, §4.1.1, §4.1.2, §4.1.3](https://github.com/openai/symphony/blob/main/SPEC.md).
- Related fork policy: [ADR-0108](0108-deep-dive-deliverables-rule.md)
  (six-deliverable rule — informs the `required_deliverables` field
  in the workflow front matter).
- Related fork policy: [ADR-0141](0141-touched-file-cleanup-rule.md)
  (touched-file lint-cleanliness — informs the `forbidden:
  blanket_nolint_suppress` field).
- Related fork policy: [ADR-0165](0165-state-md-bug-tracking.md)
  (state.md bug-status updates — informs why a tracker abstraction
  is worth the build-out).
- Research digest: [Research-0091 — Symphony SPEC review](../research/0091-symphony-spec-review.md).
- Source: `req` ("Implement 3 Symphony-inspired infrastructure
  improvements as a single PR") — paraphrased from user direction
  2026-05-09.
- Source: prior session `feedback_agents_isolated_worktree_only`,
  `feedback_verify_state_before_dispatch`,
  `feedback_deliverables_checklist_strict_parser`.
