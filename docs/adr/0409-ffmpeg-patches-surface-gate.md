<!-- markdownlint-disable MD013 MD060 -->
# ADR-0409: Automated CI gate for the ffmpeg-patches surface-sync rule (CLAUDE.md §12 r14)

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: Lusoris, Claude (Opus 4.7)
- **Tags**: `ci`, `ffmpeg-integration`, `process`, `rule-enforcement`

## Context

CLAUDE.md §12 rule 14 (introduced in
[ADR-0186](0186-vulkan-image-import-impl.md)) states: every PR that
touches a libvmaf C-API surface, a CLI flag, a `meson_options.txt`
entry, a public header, or any other interface that the in-tree
`ffmpeg-patches/` patches consume must update **the relevant patch
file in the same PR** — no exceptions. The contract protects the next
`/sync-upstream` rebase from inheriting a silently-broken FFmpeg
integration build.

Until this ADR, the rule was reviewer-eyes-only. The PR template
([`.github/PULL_REQUEST_TEMPLATE.md`](../../.github/PULL_REQUEST_TEMPLATE.md))
carried a checklist row, but no machine check verified that a header
diff actually intersected the patch-stack consumed-symbol set.
[ADR-0124](0124-automated-rule-enforcement.md) already automates the
sister rules (ADR-0108 deliverables, ADR-0100 doc-substance,
ADR-0106 ADR-backfill); §12 r14 was the last unautomated process rule
in the same family.

A drift-by-omission case (libvmaf public symbol added without a
matching patch update) becomes invisible until the next upstream sync
attempts to apply the patch stack against an FFmpeg checkout that no
longer matches. By then the contributor and reviewer who authored the
omission have moved on; the cost falls on whoever runs the next
`/sync-upstream`.

## Decision

We add a third **blocking** job —
`ffmpeg-patches-surface-check` — to
[`.github/workflows/rule-enforcement.yml`](../../.github/workflows/rule-enforcement.yml),
backed by the new
[`scripts/ci/ffmpeg-patches-surface-check.sh`](../../scripts/ci/ffmpeg-patches-surface-check.sh).
The gate parses every patch under `ffmpeg-patches/` once (sub-second
on the live nine-patch stack), extracts a "consumed set" of
`vmaf_*` / `Vmaf*` / `libvmaf_*` / `--enable-libvmaf-*` tokens, and
intersects that set against the PR's diff over public headers and
`meson_options.txt`. If the intersection is non-empty and no
`ffmpeg-patches/*.patch` is in the diff, the gate fails. Per-PR
opt-out is the line `no ffmpeg-patches update needed: REASON`, in the
same family as ADR-0108's `no <item> needed: …` syntax — covering
edge cases like a doxygen-comment fix that mentions a symbol but does
not change its signature.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Bash + grep diff parser** (chosen) | Zero new deps; mirrors `deliverables-check.sh` pattern; sub-second runtime; readable by anyone who can read shell. | Token-based regex misses some edge cases (e.g. typedef alias renames where the alias only appears in a multi-line block comment). False positives on bare type names like `VmafPicture` mentioned in any diff line — but those are *exactly* the cases that warrant a reviewer's attention. | Picked: lightest acceptable, matches the existing toolchain. |
| Python + `libclang` AST extraction | Bit-exact diff of the public C surface; no false positives from comments. | Requires `libclang` on the runner (`apt install libclang-dev`); 1–2s startup penalty; new dependency for one gate; AST parsing adds a hidden Python ≥ 3.10 floor on contributors' local checkouts. | Overkill — the gate doesn't need bit-exact symbol resolution; reviewer judgement closes any false positive in seconds via the opt-out line. |
| `ctags` / `clang-extract-sym` | Symbol set is exact; runtime fast. | Requires `universal-ctags` on the runner; output format differs across distros; brittle on `inline` / static-inline declarations in headers. | Adds a dep without enough false-positive reduction over `grep` to justify it. |
| Reviewer-only enforcement (status quo) | Zero CI cost. | Rule was reviewer-eyes-only; no mechanical guarantee. The class of bug the rule prevents is invisible until the next `/sync-upstream`, when context recovery is expensive. | Inverts the cost asymmetry — preventing one missed update saves hours of next-sync archaeology. |

The bash-only approach is intentionally *liberal* (a few false
positives on broad types like `VmafPicture` mentioned in a
comment-only diff are acceptable) because the opt-out is a one-line
PR-body addition with a stated reason. False negatives — a real
surface change slipping through — are the bug we're paying to avoid;
false positives cost ≤ 30 seconds of contributor time.

## Consequences

- **Positive**:
  - CLAUDE.md §12 r14 is now mechanically enforced. Drift-by-omission
    is impossible without a deliberate opt-out line citing a reason.
  - The check runs on every PR (`pull_request` triggers
    `[opened, edited, synchronize, reopened, ready_for_review]`),
    matching the existing rule-enforcement workflow's trigger set per
    [ADR-0331](0331-skip-ci-on-draft-prs.md).
  - Locally runnable via the same `BASE_SHA / HEAD_SHA / PR_BODY`
    contract as `deliverables-check.sh`; can be added to a future
    `make pr-check` or pre-push hook without rework.
  - Sub-second runtime — well under the workflow's 5-minute timeout
    and the global 10-second budget for diff-only gates.
- **Negative**:
  - Liberal pattern matching can fire on any header diff that
    mentions a consumed type name (e.g. a doxygen comment fix that
    happens to include `VmafPicture`). Mitigated by the per-PR
    opt-out; reviewer cost is one extra line in the PR body.
  - Adds a 19th required-status-check candidate. The aggregator job
    [ADR-0313](0313-ci-required-checks-aggregator.md) absorbs
    branch-protection bookkeeping; no manual config update is needed
    for the merge gate to honour the new check.
- **Neutral / follow-ups**:
  - Documented under
    [`docs/development/automated-rule-enforcement.md`](../development/automated-rule-enforcement.md)
    in the same PR per CLAUDE.md §12 r10.
  - Changelog fragment under `changelog.d/added/`.
  - Rebase note entry: the gate is fork-local CI, no upstream
    conflict surface, but its existence informs whoever runs the
    next sync that ffmpeg-patches integrity is now machine-checked.
  - Future enhancement: extend the consumed-set extractor to also
    parse CLI flag names declared in `core/tools/cli_parse.c`
    (currently only meson_options surfaces). Out of scope for the
    initial gate.

## References

- Source: `req` — direct user instruction, 2026-05-09: "Build a CI
  gate that auto-enforces CLAUDE.md §12 r14: every PR that changes a
  libvmaf C-API surface or a build flag must update the corresponding
  `ffmpeg-patches/` patch in the same PR. Currently the rule is
  reviewer-eyes-only."
- [ADR-0124](0124-automated-rule-enforcement.md) — parent ADR for
  the rule-enforcement workflow.
- [ADR-0186](0186-vulkan-image-import-impl.md) — origin of CLAUDE.md
  §12 r14 (the rule this ADR automates).
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — sister gate;
  established the `no <item> needed: REASON` opt-out syntax.
- [ADR-0313](0313-ci-required-checks-aggregator.md) — required-checks
  aggregator that absorbs new gates without branch-protection edits.
- [ADR-0331](0331-skip-ci-on-draft-prs.md) — `ready_for_review`
  trigger semantics inherited by the new job.
- Implementation: `scripts/ci/ffmpeg-patches-surface-check.sh`,
  `.github/workflows/rule-enforcement.yml` job
  `ffmpeg-patches-surface-check`.
