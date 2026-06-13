# ADR-0108: Every fork-local PR ships the six deep-dive deliverables

- **Status**: Accepted
- **Date**: 2026-04-18
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: docs, agents, framework, planning

## Context

A sibling-repo policy (issue #38, ported from a different lusoris repo)
codified a "deep-dive deliverables" rule: every research / hardening PR
ships a research digest, a decision matrix, an `AGENTS.md` invariant
note, a reproducer, a fork-changelog entry, and a rebase note —
config-only PRs are explicitly incomplete. This fork already has
overlapping but partial coverage:

- ADRs (per [ADR-0028](0028-adr-maintenance-rule.md) /
  [ADR-0106](0106-adr-maintenance-rule.md)) capture the *decision* and
  *alternatives*, but their audience is future-maintainer rationale, not
  research notes for the next iteration.
- The project-wide doc-substance rule (per
  [ADR-0100](0100-project-wide-doc-substance-rule.md)) covers
  *user-facing* docs, but says nothing about reproducers, rebase notes,
  or upstream-divergence tracking.
- `CHANGELOG.md` already has a "lusoris fork" section, but contributors
  do not consistently add to it, and there is no convention naming it as
  the divergence-from-upstream record.
- There is no `docs/research/` tree and no `docs/rebase-notes.md`, so
  upstream-rebase risk lives only in commit messages and reviewer
  memory.

The user directed two adaptations of the sibling-repo rule when porting
it to this repo: rename the `cauda` term back to `lusoris` / `fork` (the
original term leaked from a personal-domain naming scheme that does not
appear anywhere else in the vmaf tree), and broaden the trigger from
"feature-like changes only" to *any* fork-local PR, with the existing
backlog backfilled in the same adoption PR.

## Decision

Every PR that touches **fork-local** code, configuration, or
documentation — features, bug fixes, refactors, tooling, CI, agent
scaffolding — ships the **six deep-dive deliverables** in the same PR
as the change. *Fork-local* means anything authored by the fork that is
not a verbatim port of upstream Netflix/vmaf code (the upstream tree,
upstream CI, and upstream models are explicitly out of scope; trivial
pulls via `port-upstream-commit` are exempt). The six deliverables are:

1. **Research digest** — what was investigated and why, with source
   links to upstream docs / papers / Netflix issues / prior PRs. Lives
   at `docs/research/NNNN-kebab-topic.md` and is indexed by
   [`docs/research/README.md`](../research/README.md). One digest may
   cover multiple PRs in the same workstream; reuse by linking.
2. **Decision matrix** — when the PR picks between alternatives, the
   matrix lives in the existing ADR's `## Alternatives considered`
   section ([ADR-0106](0106-adr-maintenance-rule.md) already requires
   one ADR per non-trivial decision, so this deliverable usually
   *is* the ADR row). Trivial decisions reuse the matrix in the
   research digest.
3. **`AGENTS.md` invariant note** — when the fork-local change depends
   on an invariant that a future upstream rebase could silently drop
   (e.g., a function signature, a build-flag default, a header order),
   the relevant package's `AGENTS.md` (or the root one) gets a one-line
   "rebase-sensitive invariant" entry. Skipped for changes with no such
   invariant; the PR description states "no rebase-sensitive
   invariants" instead.
4. **Reproducer / smoke-test** — the PR description includes one
   concrete command exercising the changed path against a known input
   (a YUV from `python/test/resource/yuv/`, a CLI invocation, a
   `meson test --suite=` run, etc.). For pure-docs PRs the reproducer
   is the link-check / build command.
5. **`CHANGELOG.md` "lusoris fork" entry** — the existing
   ["Unreleased / lusoris fork"](../../CHANGELOG.md) section gets one
   bullet per merged PR describing what diverged from upstream and why.
   Release-please rolls these into the next `v3.x.y-lusoris.N` release
   notes.
6. **Rebase note** in [`docs/rebase-notes.md`](../rebase-notes.md) —
   when the change creates work that has to be re-applied or re-tested
   after the next upstream sync (e.g., a SIMD reduction that diverges
   numerically from upstream, a license-header swap on a Netflix file,
   a build-flag rename), an entry naming the affected paths and the
   re-test command. PRs that do not touch upstream-shared paths state
   "no rebase impact" in the PR description and skip the entry.

The rule applies *forward* and the existing backlog is backfilled in
the same adoption PR (a single sweep of the fork-local PRs already on
master, populating the rebase-notes ledger and adding any missing
research digests for the major workstreams). Future PRs that skip a
deliverable surface that gap in the PR description with a one-line
justification (e.g., "no rebase impact: docs-only change to
`docs/adr/`").

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the sibling-repo rule verbatim (`CHANGELOG-cauda.md`, "cauda" terminology, feature-like PRs only, no backfill) | Minimal porting cost | "cauda" is not a term in the vmaf fork (the fork uses "lusoris" everywhere — versioning, copyright, branch names); a separate `CHANGELOG-cauda.md` would compete with the existing `CHANGELOG.md` "lusoris fork" section and they would drift; "feature-like only" leaves bug fixes and CI tweaks unaudited | Per user direction (rename to lusoris/fork; reuse existing changelog; trigger on any fork-local PR; backfill backlog) |
| Rule applies only to feature-like changes (the issue's literal scope) | Smallest enforcement surface | Bug fixes that change observable behavior (e.g., the picture-pool deadlock fix in PR #32) are exactly the kind of change that creates rebase risk; excluding them defeats the rule's stated purpose | User chose "anything" |
| Skip the rule, rely on ADRs + ADR-0100 docs alone | Zero new surface | ADRs are decision-time rationale, not iteration-time research notes; ADR-0100 is user-facing docs, not rebase-impact tracking; reproducers and changelog discipline stay informal | Rejected — the gaps the issue calls out are real |
| Single mega-doc `docs/fork-deep-dive.md` instead of per-PR research digests | Low-ceremony | Merge conflicts on every PR; no per-topic git history; reads worse than one file per workstream | Rejected (same reason ADR-0028 rejected the mega-table) |
| Adopt rule going forward only (no backfill) | Cheaper this sprint | Leaves the rebase-notes ledger empty for the existing 30+ fork-local PRs, which is exactly when an upstream sync would be most painful (no record of what to re-apply) | User chose backfill |

The chosen shape uses what already exists (ADRs for decisions, the
existing `CHANGELOG.md` fork section, `AGENTS.md` for invariants,
`docs/` for research notes) instead of inventing parallel structures,
and makes the genuinely new pieces (`docs/research/`,
`docs/rebase-notes.md`, the PR-description reproducer) explicit in the
PR template so reviewers can check them at a glance.

## Consequences

- **Positive**: rebase risk is captured in a single ledger
  (`docs/rebase-notes.md`) instead of scattered through commit
  messages; iteration-time research notes have a home distinct from
  decision-time ADRs; the existing `CHANGELOG.md` "lusoris fork"
  section becomes the canonical divergence-from-upstream record;
  reviewers get a per-PR checklist; the `docs/research/` tree
  accumulates a knowledge base that survives session resets.
- **Negative**: every fork-local PR pays a per-PR cost (typically one
  changelog bullet + one rebase-notes line + a reproducer command;
  larger PRs add a research digest); reviewers must check the six
  items; the backfill sweep is one-shot but non-trivial.
- **Neutral / follow-ups**:
  - `.github/PULL_REQUEST_TEMPLATE.md` extended with a "Deep-dive
    deliverables (ADR-0108)" section listing the six checkboxes.
  - [CLAUDE.md](../../CLAUDE.md) §12 gets rule 11 (mirrored to
    [AGENTS.md](../../AGENTS.md) §12 rule 9).
  - `docs/research/` and `docs/rebase-notes.md` ship in the adoption
    PR with the backlog backfilled.
  - Future work: a lightweight CI script that grep-checks merged PR
    descriptions for the six checkboxes (or "no rebase impact" /
    similar opt-out lines). Not blocking v1 — human review carries
    the rule for now, same approach as ADR-0100.

## References

- Source: [issue #38](https://github.com/VMAFx/vmafx/issues/38) (the
  body was edited to redact the personal-domain term "cauda" that
  leaked from the porting agent; the redacted issue is the canonical
  context).
- Trigger-scope source: `req` (user, 2026-04-18 popup answer: "well i
  guess anything + filling the backlog for everything we have so
  far").
- Naming source: `req` (user, 2026-04-18: "and of course it should fit
  to the project, that was a miss of the agent not removing my domain
  from the issue ... cauda should be redacted in this issue").
- Predecessor ADRs:
  [ADR-0028](0028-adr-maintenance-rule.md) /
  [ADR-0106](0106-adr-maintenance-rule.md) — ADR-per-decision rule
  (this ADR's deliverable #2 reuses the ADR's `## Alternatives
  considered` section).
  [ADR-0042](0042-tinyai-docs-required-per-pr.md) — tiny-AI docs
  per-PR rule (specialisation of deliverable #1 / #5 for tiny-AI).
  [ADR-0100](0100-project-wide-doc-substance-rule.md) — project-wide
  doc-substance rule (the user-facing docs deliverable that this ADR
  composes with, not replaces).
  [ADR-0008](0008-readme-fork-rebrand.md) /
  [ADR-0011](0011-versioning-lusoris-suffix.md) — establish
  "lusoris" as the fork's canonical naming scheme (justifies the
  `cauda → lusoris` redaction).
