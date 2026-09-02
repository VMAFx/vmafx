# Research-0068: vmaf-tune Phase E — per-title bitrate-ladder algorithm survey

- **Date**: 2026-05-03
- **Author**: Lusoris (with Claude assistance)
- **Companion**: [ADR-0295](../adr/0295-vmaf-tune-phase-e-bitrate-ladder.md)

## Question

Given a single source clip and a sweep of (resolution, target-VMAF)
samples, what is the right algorithm to pick `n` ABR ladder rungs?

## Survey

### Netflix per-title encoding (2015 paper)

Sample (resolution, bitrate) on a grid; compute the upper convex hull
of (bitrate, vmaf); the hull *is* the ladder. Industry baseline. Two
properties that matter:

1. **Pareto frontier** — for any bitrate, the hull rendition has the
   maximum VMAF; nothing else dominates.
2. **Diminishing returns** — the slope (Δvmaf / Δbitrate) is
   monotonically non-increasing along the hull. ABR clients stepping
   up rungs see ever-smaller quality gains for ever-larger bitrate
   spends.

### Apple HLS Authoring Specification §2.3

A *fixed* recommended ladder, geometric in bandwidth (×2 per rung).
Trivial to implement; useful as a default for content where the source
isn't known. Equivalent to ignoring the title — defeats the per-title
premise.

### JND-spaced (Visicom 2019, JND-VMAF / JND-aware ABR)

Pick rungs so each adjacent pair differs by ~6 VMAF (one
just-noticeable-difference). Perceptually motivated; requires a JND
model. We don't have one — would layer on top of VMAF if we did.
Deferred.

### Bayesian-optimisation sampler

Replace the resolution-grid × target-vmaf sweep with sequential BO
querying. Fewer encodes per title; principled exploration. Orthogonal
to the *picker* — the ladder math runs on whatever sample cloud the
sampler produces. Deferred.

### av1an `--target-quality`

Per-rendition bisect. Conceptually a Phase B sibling, not a Phase E
sibling — the sampler half of the pipeline. We already wire that as
Phase B (PR #347).

## Decision

Implement the Netflix two-pass approach: Pareto filter, then upper-
convex envelope with `cross >= 0` pop predicate (drops accelerating-
returns interior points so the hull is everywhere concave /
diminishing-returns). Pluggable sampler — production wires Phase B's
bisect; tests inject a synthetic stub. Default rendition picker uses
log-bitrate spacing (Apple HLS authoring spec convention) with VMAF
spacing as an opt-in for perceptual ABR work.

## Open questions

- Real-corpus PLCC of the picked ladder against Netflix's published
  per-title rungs. Gated on Phase B merging (so we have a real
  sampler) plus a Netflix-Public encode of N representative titles.
  Deferred to a follow-up validation digest.
- Whether to add a JND-spaced rung-picker once we ship a JND head
  (tiny-AI roadmap). Out of scope for Phase E.

## References

- Netflix per-title encoding paper (2015) — the canonical reference.
- Apple HLS Authoring Specification for Apple Devices §2.3.
- av1an / ab-av1 — prior art for `--target-quality` per-rendition
  bisect (Phase B, not Phase E).
- Bitmovin Per-Title — closed-source equivalent.
- PR #354 capability audit (`docs/research/0061-vmaf-tune-capability-audit.md`,
  pending) Bucket #6 — the source flagging this as the fork's
  highest-leverage gap.
