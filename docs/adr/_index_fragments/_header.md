# Architectural Decision Records (ADR)

This is the **canonical, tracked** decision log for the fork. Every non-trivial
architectural / policy / scope decision lands here as its own markdown file
before the corresponding commit merges.

## Format

We use [Michael Nygard's ADR format](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions)
(MADR-style), one markdown file per decision — not a mega-table. See
[joelparkerhenderson/architecture-decision-record](https://github.com/joelparkerhenderson/architecture-decision-record)
for background.

Each ADR file is named `NNNN-kebab-case-title.md` with a zero-padded 4-digit ID
and follows the structure in [0000-template.md](0000-template.md):

```markdown
# ADR-NNNN: <short, declarative title>

- **Status**: Proposed | Accepted | Deprecated | Superseded by [ADR-NNNN](NNNN-title.md)
- **Date**: YYYY-MM-DD
- **Deciders**: <names / handles>
- **Tags**: <comma-separated area tags>

## Context              — the problem, the forces at play
## Decision             — one paragraph in active voice
## Alternatives considered  — at minimum the runner-up, in a pros/cons table
## Consequences         — Positive / Negative / Neutral-follow-ups
## References           — upstream docs, prior ADRs, related PRs, popup-answer source
```

## Conventions

- **Filename**: `NNNN-kebab-case-title.md`. IDs are assigned in commit order
  and never reused.
- **Immutable once Accepted**: the body is frozen. To change a decision, write
  a new ADR with `Status: Supersedes ADR-NNNN` and flip the old one to
  `Superseded by ADR-MMMM`.
- **One decision per ADR** — if you find yourself writing "and also…", split it.
- **Tagging**: use the flat tag palette below so `grep -l 'Tags:.*cuda'
  docs/adr/*.md` works. New tags are fine when justified.
- **Link from per-package AGENTS.md**: the relevant per-package `AGENTS.md`
  points to the ADRs that govern that subtree, so the rationale is one click
  away from the code.
- **Backfill policy**: ADRs ≤ 0099 are *backfills* — decisions made before
  the ADR practice was formalised on 2026-04-17, captured retroactively from
  commit history and planning dossiers. Their `Status` reflects the current
  code, not the original decision date. New decisions start at 0100.

### Tag palette

`ai`, `agents`, `build`, `ci`, `claude`, `cli`, `cuda`, `dnn`, `docs`, `framework`,
`git`, `github`, `license`, `lint`, `matlab`, `mcp`, `planning`, `python`,
`readme`, `release`, `security`, `simd`, `supply-chain`, `sycl`, `testing`,
`workspace`.

## Why it exists

A Claude session makes a decision (directory move, CI gate change, dependency
swap), commits it, the session ends, and the rationale is recoverable only from
the commit message — which typically summarises the *what* but omits the
*alternatives considered*. ADRs preserve "we chose X over Y because Z" in a
single auditable place. See [ADR-0028](0028-adr-maintenance-rule.md).

## What counts as non-trivial?

Another engineer could reasonably have chosen differently. Examples:

- Directory moves (e.g., [ADR-0026](0026-workspace-relocated-under-python.md):
  `workspace/` → `python/vmaf/workspace/`)
- Base-image / dependency policy (e.g.,
  [ADR-0027](0027-non-conservative-image-pins.md): non-conservative CUDA pins)
- CI-gate semantics (e.g., [ADR-0024](0024-netflix-golden-preserved.md):
  Netflix golden tests as required status)
- Test-selection / regeneration rules
- Coding-standards changes (e.g.,
  [ADR-0012](0012-coding-standards-jpl-cert-misra.md))
- New user-visible flags or surfaces (e.g.,
  [ADR-0023](0023-tinyai-user-surfaces.md))

**Not** ADR-worthy: bug fixes, implementation details, one-off refactors that
don't change any interface or policy.

## Relation to `.workingdir2/`

Planning dossiers live under `.workingdir2/` (gitignored). Mirrored copies of
ADRs may exist there for local session continuity, but the tracked
`docs/adr/` tree is authoritative.

## Index

| ID | Title | Status | Tags |
| --- | --- | --- | --- |
