<!-- markdownlint-disable MD060 -->
# ADR-0308: Encoder knob-sweep recipe-regression revision policy

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: Lusoris, Claude
- **Tags**: ai, vmaf-tune, codec-adapters, knob-sweep, fork-local

## Context

[ADR-0305](0305-encoder-knob-space-pareto-analysis.md) commits the
fork to per-slice Pareto stratification on the 12,636-cell knob sweep
and ships a regression detector
([`ai/scripts/analyze_knob_sweep.py`](../../ai/scripts/analyze_knob_sweep.py))
that flags recipes losing VMAF against the bare encoder default at
matched bitrate within a slice. The policy question ADR-0305 left
open is **what to do with the regressions once they are detected**:
the analyser produces 1,915 flagged rows on the populated sweep
([Research-0080](../research/0080-encoder-knob-sweep-findings.md)),
clustered around `h264_nvenc + bf3 / spatial_aq / full_hq` under
CBR/VBR, plus a smaller hevc_nvenc + spatial_aq cluster.

The corpus-content invariance of the top-15 aggregated cells (every
top-cell regression hits all 9 corpus sources) makes those cells
*structural* — they reflect codec/rc-mode interactions, not content
flukes — and therefore actionable. The CQP regression rate (6.6 %) is
a third of the CBR rate (20.2 %) and confirms the core
[Research-0063](../research/0063-encoder-knob-space-cq-vs-vbr-stratification.md)
finding; gating only on CQP would mask the CBR/VBR regressions, so
the policy must apply across all rc modes.

`tools/vmaf-tune/codec_adapters/*` does not currently bake recipes as
adapter-level defaults — recipes were enumerated by
`hw_encoder_corpus.py` for sweep coverage. The policy here therefore
governs **future** adapter and recommend-API behaviour, not a current
default that needs walking back.

## Decision

We will adopt the following recipe-revision policy for the fork:

1. A recipe regression is *structural* iff it reproduces on **at
   least 7 of 9** corpus sources within a single
   `(codec, rc_mode, recipe, preset, q)` cell. Structural regressions
   are forbidden as adapter-level defaults and forbidden as
   `vmaf-tune recommend` outputs without an explicit override.
2. A recipe regression is *content-dependent* iff it reproduces on
   1-6 of the 9 sources. Content-dependent regressions are recorded
   in the per-slice CSV (already produced by ADR-0305) but do not
   trigger an adapter-level revision; the `vmaf-tune recommend` path
   filters them at recommend-time using the per-slice hull lookup.
3. The four structural patterns identified in
   [Research-0080 §Recipe-revisions](../research/0080-encoder-knob-sweep-findings.md#recipe-revisions-proposed-for-follow-up-prs)
   are **acknowledged as ship-blockers** for any future adapter
   default; follow-up PRs that wire them out land **per-codec** so
   each revision carries its own bisect signal.
4. The regression detector becomes a **non-CI** local gate for now —
   the sweep is too expensive to run in CI (3 hours, ~2 GiB JSONL,
   single-host variance). Re-running it after a sweep refresh is
   tracked in [`docs/rebase-notes.md` §0308](../rebase-notes.md).
   Promotion to a CI gate is deferred until a smaller stratified
   sample (e.g. 1 source × all codecs × all rc modes ≈ 1,400 cells)
   reproduces the structural patterns at acceptable wall time;
   sample design is out of scope here.
5. Producer-side schema alignment (rename `src` → `source`,
   `actual_kbps` → `bitrate_kbps`, `vmaf` → `vmaf_score`,
   `enc_ms` → `encode_time_ms`, `recipe == 'bare'` →
   `is_bare_default`) lands in a follow-up PR that bumps
   `SCHEMA_VERSION` from 2 to 3. Until then, analysis runs go through
   a throw-away wrapper that performs the rename in-process; this is
   acceptable because `comprehensive.jsonl` is gitignored and the
   rename is purely cosmetic.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|--------|------|------|----------------|
| **A. Adopt structural-only policy with a 7-of-9 threshold (chosen)** | Filters out content flukes; the 7-of-9 cut matches the natural cluster (top-15 cells all hit 9-of-9, no observed cells in 4-6 range); each forbidden recipe has a single-paragraph justification. | Threshold is empirical and could shift if the corpus grows past 9 sources; need to revisit the absolute threshold then (likely to a fraction). | Best fit for the corpus shape we actually have. The structural cluster is sharp (9-of-9 vs 0-of-9), so the 7-of-9 cut is robust to one or two follow-up reruns. |
| **B. Forbid every detected regression** (no source-count threshold) | Maximally cautious; never lets a regressing recipe through. | Produces 1,915 ship-blockers, most of which are single-source flukes that the per-slice hull lookup already filters at recommend-time. Operationally impossible to maintain. | Drowns the load-bearing structural findings in noise; bisect signals would point at a follow-up that flips a single-source fluke instead of the real cell. |
| **C. Accept all regressions; rely on per-slice hull lookup at recommend-time only** | Zero adapter changes; aligns with current "recipes are sweep-coverage, not adapter defaults" reality. | Loses the structural-finding signal entirely. Future adapter work that promotes `recipe=bf3` as a default for h264_nvenc would silently regress every NVENC user; nothing in the package would block it. | Concedes the entire ADR-0305 invariant. The whole point of the sweep was to drive adapter defaults; accepting regressions means the sweep was busywork. |
| **D. Promote the detector to a CI gate immediately on a synthetic 20-row fixture** | Catches obvious regressions in the analyser-script logic itself. | Doesn't catch real regressions — the synthetic fixture cannot reproduce the corpus-stable 9-of-9 patterns. The 20-row test in `ai/tests/test_knob_sweep_analysis.py` already covers the script logic and is the right surface for that gate; mixing it with recipe policy confuses the two. | The script logic is gated separately; the *policy* gate needs the real corpus and is therefore offline-only. Promoting it would create a green-CI false comfort. |

## Consequences

- **Positive**:
  - Structural-finding ship-blockers are documented in one place
    (this ADR + Research-0080 §Recipe-revisions); follow-up adapter
    PRs cite the cell directly.
  - The 7-of-9 threshold is reproducible — a re-run of
    `analyze_knob_sweep.py` on a refreshed sweep applies the same cut
    without further policy work.
  - CQP recipes get the breathing room they earned: the 6.6 %
    regression rate makes them safer adapter-default candidates than
    CBR/VBR (re-confirms Research-0063).
- **Negative**:
  - The 1,915 raw regressions are **not** all forbidden — only the
    structural subset is. Reviewers must look up the source-count
    column before reading too much into a flagged row.
  - The producer-side schema rename remains tech debt until the
    SCHEMA_VERSION-3 follow-up; current analysis runs go through a
    throw-away wrapper.
  - No CI gate yet; the policy is review-time, not commit-time. A
    rebase that drops the structural ship-blocker from this ADR
    silently loses the gate.
- **Neutral / follow-ups**:
  - Per-codec adapter revisions land as separate PRs (one per codec)
    for clean bisect signals.
  - Smaller stratified-sample CI design is tracked under future
    research-NN once the structural cluster has held across at least
    one sweep refresh.
  - `ai/AGENTS.md` carries the invariant note pointing at this ADR
    plus the underlying ADR-0305 invariant.

## References

- [ADR-0305](0305-encoder-knob-space-pareto-analysis.md) — per-slice
  Pareto stratification methodology (PR #400).
- [Research-0063](../research/0063-encoder-knob-space-cq-vs-vbr-stratification.md)
  — CQ-vs-VBR finding that motivated per-slice analysis.
- [Research-0077](../research/0077-encoder-knob-space-pareto-frontiers.md)
  — analysis scaffold methodology (PR #400).
- [Research-0080](../research/0080-encoder-knob-sweep-findings.md) —
  populated findings + recipe-revision proposals (this PR).
- [ADR-0237](0237-quality-aware-encode-automation.md) — Phase A
  harness that produced the sweep.
- [ADR-0297](0297-vmaf-tune-codec-dispatcher.md) — multi-codec
  dispatcher used for the sweep.
- [ADR-0301](0301-vmaf-tune-sample-clip.md) — `--sample-clip-seconds`
  mode used to keep sweep wall time tractable.
- Source: `req` (direct user direction in this session: "identify any
  `tools/vmaf-tune/codec_adapters/*` recipe defaults that regress vs
  the bare encoder at matched bitrate within a slice"; threshold
  policy paraphrased from the user's structural-vs-fluke framing).
