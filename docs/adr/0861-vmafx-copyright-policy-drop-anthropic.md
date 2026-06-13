# ADR-0861: Drop "and Claude (Anthropic)" from fork copyright lines

- **Status**: Accepted
- **Date**: 2026-05-30
- **Deciders**: lusoris
- **Tags**: license, docs, governance

## Context

The fork's wholly-new files have carried the line
`Copyright 2026 Lusoris and Claude (Anthropic)` since ADR-0025 / ADR-0105
formalised the dual-notice pattern. The original intent was to acknowledge
that Claude (Anthropic) authored or co-authored the file content during
agent-driven sessions.

A copyright notice, however, asserts a *legal rights claim*. Anthropic is
not a rights holder in this codebase: the company does not own the output
of its model under its own terms of service, and the user (the human
author who directs the session) is the sole human rights holder under the
fork's BSD-3-Clause-Plus-Patent license. Carrying Anthropic in the
copyright line therefore overstates the legal position and may confuse
downstream consumers about who can be approached for relicensing or
attribution exceptions.

The VMAFx rebrand (2026-05-27 decisions) is the natural moment to align
the copyright notice with the legal reality.

## Decision

We will drop "and Claude (Anthropic)" from the fork's copyright line.
Wholly-new fork files now carry `Copyright 2026 Lusoris` (single notice)
instead of `Copyright 2026 Lusoris and Claude (Anthropic)` (dual notice).

The change is applied:

1. To every existing in-tree file with the dual-notice string, except
   files preserved for audit-trail or rebase-stability reasons (see
   *Alternatives considered* below).
2. To the rule text in `CLAUDE.md` §12 r7 and `CONTRIBUTING.md` so future
   files are created with the single-notice form.

ADR-0025 and ADR-0105 (the original dual-notice ADRs) remain on-record
as the historical decision; this ADR supersedes their *copyright-line
format* recommendation only — the rest of those ADRs (Netflix header
preservation, year-range bumping, license identifier) stays in force.

The credit Claude (Anthropic) used to receive in the copyright line is
preserved in commit-message `Co-Authored-By:` trailers, which is the
appropriate surface for authorship acknowledgement that is not a legal
rights claim.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the dual notice indefinitely | Zero churn; preserves the historical acknowledgement style; matches what's already in tree | Asserts a legal rights claim Anthropic does not actually hold; risks confusing downstream consumers about who to contact for licensing exceptions | Rejected — overstates the legal position. |
| Replace with `Copyright 2026 Lusoris` (chosen) | Aligns the notice with the actual rights holder; trivial substitution; keeps acknowledgement available via commit trailers | One-time churn across ~744 files; supersedes the format guidance in ADR-0025 / ADR-0105 | Chosen. |
| Replace with `Copyright 2026 Lusoris (with assistance from Claude (Anthropic))` | Keeps the acknowledgement visible in-file | Still potentially misread as a rights claim; verbose; non-standard SPDX-friendly format | Rejected — the goal is to avoid the rights-claim implication, not to phrase it differently. |
| Drop the line entirely and rely on git history + LICENSE | Minimal text in tree | Loses the per-file copyright marker that downstream LICENSE auditors look for | Rejected — the per-file notice is useful even without the dual party. |

## Consequences

- **Positive**: Copyright line accurately reflects the legal rights
  holder. Removes a potential downstream-confusion vector.
  Single-notice form is shorter and simpler.
- **Negative**: One-time large diff (~744 files, mechanical 1-line
  changes) across the tree. ADR-0025 / ADR-0105's format guidance is
  partially superseded — readers of those ADRs need this ADR for the
  current rule.
- **Neutral / follow-ups**:
  - The following files are preserved at the dual-notice form for
    audit / rebase stability and are out of scope for this ADR:
    - `AGENTS.md` and `core/AGENTS.md` — owned by in-flight PR #234
      (rebase invariant).
    - `docs/rebase-notes.md` — owned by in-flight PR #321.
    - `docs/adr/0025-copyright-handling-dual-notice.md`,
      `docs/adr/0105-copyright-handling-dual-notice.md`,
      `docs/adr/0312-ffmpeg-patches-vmaf-tune-integration.md` — ADR
      bodies are immutable once Accepted (CLAUDE.md global-rules
      ADR-maintenance rule).
    - `docs/research/0003-ssimulacra2-port-sourcing.md` — research
      audit trail.
    - `ffmpeg-patches/0002-add-vmaf_pre-filter.patch`,
      `ffmpeg-patches/0007-libvmaf-tune-qpfile-unified.patch`,
      `ffmpeg-patches/0008-add-libvmaf_tune-filter.patch` —
      ffmpeg-patches embed git-index hashes; the patches will adopt
      the new notice the next time they are regenerated against
      master via `refresh-ffmpeg-patches`.
  - The deferred files listed above carry the historical notice; this
    is expected and not a lint violation.
  - New ffmpeg-patch regeneration after this PR lands will pick up the
    single-notice form automatically.

## References

- ADR-0025: Copyright handling — dual-notice pattern (partially
  superseded by this ADR for the copyright-line format).
- ADR-0105: Copyright handling — dual-notice pattern (Phase 2
  restatement; partially superseded by this ADR).
- Companion in-tree memory:
  `/home/kilian/.claude/projects/-home-kilian-dev-vmaf/memory/project_copyright_lusoris_only.md`
  (records the 2026-05-27 lock).
- Source: `req` — user direction 2026-05-27, recorded in the rebrand
  decisions: "drop 'and Claude (Anthropic)' from fork copyright lines.
  Anthropic is not a rights holder."
