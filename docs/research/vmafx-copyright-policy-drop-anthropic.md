# Research digest — VMAFx copyright policy: drop "and Claude (Anthropic)"

- **Date**: 2026-05-30
- **Companion ADR**: [ADR-0861](../adr/0861-vmafx-copyright-policy-drop-anthropic.md)
- **Companion PR**: `chore/vmafx-rebrand-urls-and-copyright`

## Question

The fork's wholly-new files have carried
`Copyright 2026 Lusoris and Claude (Anthropic)` since the 2026-02
dual-notice formalisation (ADR-0025 / ADR-0105). The user direction
on 2026-05-27 (VMAFx rebrand kickoff) was to drop the
"and Claude (Anthropic)" half: "Anthropic is not a rights holder."
We need to know what *kind* of substitution is correct, what surfaces
must follow, and which surfaces must stay frozen.

## Findings

### F1 — Legal status of model output under Anthropic terms of service

Anthropic's commercial terms (as of the 2025-2026 SLA in effect when
this fork was scaffolded) explicitly assign output ownership to the
end-user, not to Anthropic. The model is treated like a tool, not a
co-author. The dual-notice form therefore overstates Anthropic's legal
position: there is no copyright stake to assert in fork-authored
files.

This makes the user direction consistent with the upstream legal
framing — we are not removing a real claim, we are removing a
misleading marker.

### F2 — License-identification tooling consequences

SPDX-style scanners (`scancode-toolkit`, `licensee`, GitHub's own
license-detection) parse the per-file copyright block to attribute
files. The current dual-notice form decodes to two parties:

```text
Copyright 2026 Lusoris and Claude (Anthropic)
SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
```

Scanners that key on the copyright string surface "Claude (Anthropic)"
as a co-rights-holder, which is wrong. Single-notice form removes that
false signal cleanly; SPDX identifier is unchanged so the license
attribution itself remains correct.

### F3 — Co-authorship credit preservation

Removing "Claude (Anthropic)" from the copyright line does not erase
the agent's authorship trail. Every commit on the fork already carries
a `Co-Authored-By: Claude Opus N <noreply@anthropic.com>` trailer.
That trailer is:

- the conventional GitHub surface for co-authorship credit,
- legally non-binding (no rights claim implied),
- already enforced by the project's commit-message hook.

So the move is *from* an inappropriate legal surface (copyright line)
*to* an appropriate credit surface (commit trailer) — nothing is lost.

### F4 — ADR-0025 / ADR-0105 immutability handling

The CLAUDE.md global ADR-maintenance rule freezes ADR bodies on
status=Accepted. ADR-0025 and ADR-0105 explicitly recommend the dual
notice. The clean way to retire that recommendation is a *partial
supersession*: a new ADR (this one — ADR-0861) that supersedes only
the copyright-line *format* clause, leaving the rest (Netflix header
preservation, year-range bumping, SPDX identifier) in force. ADR-0861
must cross-link both predecessors in its `## References` section, and
the ADR-README index row must reflect the partial-supersedure status.

### F5 — File-level scope: what gets touched vs. preserved

A repo-wide grep for the dual-notice string returns ~755 files (744
fork-local + 8 audit-preserved + 3 ffmpeg-patches). Substitution rules:

- **Substitute (744 files)**: any fork-local source, test, or
  doc that authors the dual-notice string as a per-file marker.
- **Preserve verbatim (8 files)**:
  - `AGENTS.md`, `core/AGENTS.md` — owned by in-flight PR #234
    (rebase invariant; merging there avoids a conflict).
  - `docs/rebase-notes.md` — owned by in-flight PR #321.
  - `docs/adr/0025-*.md`, `docs/adr/0105-*.md`,
    `docs/adr/0312-*.md` — ADR-body immutability rule.
  - `docs/research/0003-ssimulacra2-port-sourcing.md` — research
    audit-trail rule.
- **Defer (3 ffmpeg-patch files)**: the patches embed git-index
  hashes (`index aaaaaaa..bbbbbbb`) for git-am compatibility against
  `n8.1`; editing patch hunks invalidates those hashes and breaks
  the apply gate. Patches will pick up the single-notice form the
  next time they are regenerated via `refresh-ffmpeg-patches`,
  which recomputes the hashes from the post-rebase source tree.

### F6 — URL-sweep residuals (out of scope but adjacent)

The 2026-05-28 `T-POST-CUTOVER-URL-SWEEP` swept `lusoris/vmaf` →
`VMAFx/vmafx` across 113 files. A repo-wide grep on 2026-05-30 finds
4 remaining occurrences:

- `docs/state.md` — historical citation of the sweep (preserve).
- `docs/rebase-notes.md` — historical citation (preserve, owned by
  PR #321).
- `changelog.d/changed/post-cutover-url-sweep.md` — historical
  citation (preserve).
- `docs/research/release-preview-3.1.0-lusoris.0.md` — **live**
  `--repo-url=lusoris/vmaf` in a copy-pasteable reproduce-locally
  block. This one is a real bug and gets fixed in this PR.

## Decision

Single notice: `Copyright 2026 Lusoris` for all fork-local files
going forward (see ADR-0861 for the full record and the alternatives
matrix).

## Reproducer

```bash
# Confirm pre-PR count of dual-notice files (excluding preserved set)
git ls-files | xargs grep -l 'Copyright 2026 Lusoris and Claude' \
  | grep -vE '(^|/)AGENTS\.md$|^docs/rebase-notes\.md$|^docs/adr/(0025|0105|0312)-|^docs/research/0003-ssimulacra2-port-sourcing\.md$|^ffmpeg-patches/' \
  | wc -l
# Expected: 744 on master, 0 after this PR merges.

# Confirm preserved-set is untouched after merge
git diff master --stat AGENTS.md core/AGENTS.md \
  docs/rebase-notes.md \
  docs/adr/0025-copyright-handling-dual-notice.md \
  docs/adr/0105-copyright-handling-dual-notice.md \
  docs/adr/0312-ffmpeg-patches-vmaf-tune-integration.md \
  docs/research/0003-ssimulacra2-port-sourcing.md \
  ffmpeg-patches/0002-add-vmaf_pre-filter.patch \
  ffmpeg-patches/0007-libvmaf-tune-qpfile-unified.patch \
  ffmpeg-patches/0008-add-libvmaf_tune-filter.patch
# Expected: no output (preserved-set untouched).

# Confirm URL leftover is gone
git grep -n 'lusoris/vmaf' -- ':!docs/state.md' ':!docs/rebase-notes.md' \
  ':!changelog.d/changed/post-cutover-url-sweep.md'
# Expected: no output.
```

## Open questions

None. The user direction (2026-05-27) and the two preceding ADRs
(0025, 0105) together fully specify the surface. The PR mechanically
applies the substitution; the deferred sets are documented in
ADR-0861 §Consequences.
