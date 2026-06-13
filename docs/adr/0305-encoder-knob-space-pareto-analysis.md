<!-- markdownlint-disable MD013 MD060 -->
# ADR-0305: Encoder knob-space Pareto-frontier analysis stratified per (source, codec, rc_mode)

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: lusoris, Claude
- **Tags**: ai, vmaf-tune, research, encoder, pareto, fork-local

## Context

[Research-0063](../research/0063-encoder-knob-space-cq-vs-vbr-stratification.md)
established empirically that the "VOD-HQ" recipe NVENC and QSV ship in their
documentation is calibrated for VBR/CBR rate control: at fixed CQ it
*regresses* NVENC by 2.7–3.3 mean VMAF and barely lifts QSV (+0.2 to +0.9),
while at fixed bitrate the same recipe is expected to lift quality.
Rate-control mode is therefore a first-class axis of the recipe space, not
a free parameter that integrates out across a global Pareto frontier.

That finding generalises beyond NVENC: every encoder family (libx264 /
libx265 / libsvtav1 / libaom-av1 / libvpx-vp9 / libvvenc, plus the hardware
families NVENC / QSV / AMF / VideoToolbox already wired into vmaf-tune)
has knobs whose direction-of-improvement flips with rate-control mode and
with content type. A single global Pareto frontier on (bitrate, vmaf)
collapses those flips and produces "consensus" recipes that lose to the
bare encoder defaults on any individual slice.

We are running a 12,636-cell knob-sweep — 9 Netflix sources × 6 codec
families × 3 rate-control modes (cq, vbr, cbr) × ~78 knob combinations per
codec — to provide the dominance hull that should drive `vmaf-tune
recommend` defaults for the 17 in-flight codec adapter PRs. The sweep is
generated locally and stored under `runs/phase_a/full_grid/` (gitignored;
~2 GiB JSONL). Sweep ETA at the time of this PR is ~3 hours.

This PR is the methodology + analysis scaffold. The actual headline
findings land via a follow-up commit on the same branch when the sweep
completes.

## Decision

We will compute Pareto frontiers **per (source, codec, rc_mode) slice**
rather than as a single global frontier. For each slice the analysis
script computes a 2-D dominance hull on (bitrate_kbps, vmaf_score), then
applies (encode_time_ms) as a tiebreaker on the hull boundary. Recipes
that fall on the hull for at least one slice qualify as
ship-candidates; recipes that *regress* against the bare-encoder default
at matched bitrate within the same slice are blocked from shipping as
adapter defaults.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-(source, codec, rc_mode) stratified Pareto** (chosen) | Preserves the rate-control flip Research-0063 identified; surfaces content-type interactions (low-motion vs sports vs animation); directly answers the question "what should the adapter default be in mode X". | ~25 frontiers to maintain (9 × 6 × 3 / dedup); larger reporting surface; per-slice hulls are sparser (more knob combos appear on at least one hull, complicating "ship one default" decisions). | — |
| Single global Pareto on (bitrate, vmaf) | One frontier to read; single ship-candidate recipe per encoder. | Collapses the CQ vs VBR flip into a wash; the "consensus" recipe loses to bare defaults on every individual slice (Research-0063 §Result). Hides content-type variance entirely. | Empirically regresses NVENC h264/hevc by ~4 VMAF at cq=30 against the bare default — exactly the failure mode the global hull would reproduce. |
| Codec-only stratified Pareto (one frontier per codec, rc_mode and source pooled) | Smaller reporting surface (6 frontiers); still stratifies the codec axis. | Re-introduces the rate-control flip *within* each frontier, so the hull bakes in the CQ-mode regression Research-0063 calls out. Equivalent to the global option for the rate-control failure mode. | Doesn't fix the load-bearing problem (rc_mode flips); the per-codec aggregate hides which mode was responsible for each hull point. |

## Consequences

- **Positive**: Adapter-default recipes are anchored to evidence on the
  exact (codec, rc_mode) the user is targeting; the regression-detection
  invariant in `ai/AGENTS.md` (knob-sweep corpus invariant) becomes
  testable per-slice rather than as a noisy global average.
- **Positive**: Future per-codec adapter PRs (svtav1, vvenc, aom-av1,
  vpx-vp9, x265 — the ones still missing from the codec_adapters
  registry) ship with a known-good recipe-per-rc-mode triple instead
  of inheriting "VOD-HQ" guesses from upstream documentation.
- **Negative**: ~25 frontier reports to maintain; the per-slice CSV
  tables are voluminous and the markdown summary is the load-bearing
  reading surface. Headline-finding stability across reruns is bounded
  by the sweep's per-frame VMAF noise floor (~0.1–0.3 points).
- **Neutral / follow-ups**: When the sweep completes, a follow-up
  commit on this branch flips Research-0077 §Headline findings from
  TBD to the per-slice tables and writes the per-codec recipe
  shortlist into `tools/vmaf-tune/`'s adapter recipe surface
  (separate PR, gated on the regression-detection check passing for
  every recipe in the shortlist).

## References

- [Research-0063](../research/0063-encoder-knob-space-cq-vs-vbr-stratification.md) —
  CQ vs VBR stratification finding (companion to this ADR).
- [Research-0077](../research/0077-encoder-knob-space-pareto-frontiers.md) —
  this ADR's research digest (methodology + scaffolded findings).
- [ADR-0237](0237-quality-aware-encode-automation.md) — `vmaf-tune` Phase A scope.
- [ADR-0291](0291-fr-regressor-v2-prod-ship.md) — `fr_regressor_v2` v2 prod ship.
- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware FR regressor + closed-vocab invariant.
- [ADR-0297](0297-vmaf-tune-encode-multi-codec.md) — codec-agnostic encode dispatcher.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables rule.
- Source: `req` — user request to ship the methodology + analysis scaffold
  ahead of sweep completion so the recipe-default workstream is unblocked.
