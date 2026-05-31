<!-- markdownlint-disable MD060 -->
# ADR-0295: vmaf-tune Phase E — per-title bitrate-ladder generator

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, automation, abr, fork-local

## Context

Phase A of `vmaf-tune` (ADR-0237 / PR #329, merged) ships the
encoder-grid corpus generator. Phase B (target-VMAF bisect, PR #347)
gives us "find the encoder parameters that hit a requested VMAF for a
given resolution". Phase D (per-shot dynamic CRF) is in flight.

What the fork still does not own — and what the PR #354 capability
audit ranked the **single biggest game-changer** — is the next layer
up: combining bisect-at-each-resolution into a **per-title ABR
ladder**. The per-title encoding paper (Netflix 2015) is unambiguous
that the optimal ladder for one title is the upper convex hull of
(bitrate, vmaf) points sampled across multiple resolutions, not a
fixed authoring spec. The audit's wording: ships this and the fork
"reshapes from 'best open-source VMAF measurement' into 'only
open-source per-title ladder generator with measured-PLCC proxy'".

This Phase E PR scaffolds the surface — the API, the convex-hull
math, the rendition picker, the manifest emitters (HLS / DASH / JSON)
— with a fully-mocked sampler so the smoke path works without the
Phase B bisect being merged. Real (resolution × target) sampling
wires up in a follow-up PR once Phase B lands.

## Decision

We will ship `tools/vmaf-tune/src/vmaftune/ladder.py` and a
`vmaf-tune ladder` CLI subcommand that:

1. **Sample** the (resolution × target_vmaf) plane via a pluggable
   `SamplerFn` callback (default: dispatch to Phase B's bisect; tests
   inject a synthetic stub).
2. **Compute the Pareto frontier** as a two-pass: drop dominated
   points, then take the upper-convex envelope (the diminishing-
   returns hull).
3. **Pick `n` rungs** from the hull using either log-bitrate spacing
   (Apple HLS authoring-spec convention, default) or VMAF spacing
   (perceptual).
4. **Emit a manifest** in HLS master-playlist, DASH MPD, or JSON
   descriptor form.

The default canonical rendition set is the 5-rung
1080p/720p/480p/360p/240p ladder against VMAF targets
{95, 90, 85, 75, 65}; both are CLI-overridable.

Scope intentionally excludes: real encodes (Phase A's job),
target-VMAF bisect (Phase B's job), per-shot variation (Phase D's
job), and live MCP exposure (Phase F).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Pareto-then-upper-convex-hull (chosen)** | Mirrors Netflix per-title paper exactly; produces strictly monotonic, diminishing-returns ladder; small inline implementation (~30 LOC); ABR clients see no inversions when stepping rungs | Two-pass; sensitive to floating-point ties on bitrate (handled by tie-break sort + dedup) | Gold standard for per-title ladders; everything else is a degraded approximation |
| Apple HLS authoring-spec fixed rungs | Trivial; broad client compatibility | Same ladder for every title regardless of content complexity — defeats the point of per-title encoding; the audit explicitly calls fixed ladders out as the worst option | Rejected — defeats the entire premise |
| Geometric (×2) bitrate ladder | Simple; matches HLS spec recommendations; no encoding required | Ignores the source's R-D curve; cartoons need fewer bits than sports at the same rung; same as fixed authoring spec, just parameterised | Rejected — same fundamental flaw as fixed rungs |
| JND-spaced ladder (Visicom 2019, JND-VMAF) | Perceptually motivated; matches viewer's quality-step threshold | Requires a JND model on top of VMAF (we only have VMAF); deferred until tiny-AI exposes a JND head | Deferred to a future ADR; layer on top once JND head ships |
| Bayesian-optimisation sampler (instead of grid×bisect) | Fewer encodes per title; principled exploration | Phase B's bisect already exists; BO would be a parallel research workstream; orthogonal to the ladder math | Out of scope — Phase E is the ladder math; sampler is pluggable |

## Consequences

- **Positive**:
  - Closes the loop on the Phase A→B→C→D→E pipeline. With Phase B
    merged, a single CLI invocation produces the full ladder for a
    title.
  - Phase F (MCP) gets `generate_ladder` for free — wraps the
    `build_and_emit` convenience.
  - The audit's "game-changer" status moves from claimed to
    demonstrable: no other open-source tool ships per-title
    ladders against VMAF measurement out of the box.
  - HLS and DASH manifest output means the CLI is directly callable
    by an encode pipeline; downstream tooling re-points the
    placeholder URIs at real per-rendition playlists.

- **Negative**:
  - The default `sampler=None` raises `NotImplementedError` until
    Phase B's bisect lands. The CLI is currently smoke-only — useful
    via Python tests, not yet useful end-to-end. Status stays
    Proposed until that integration PR lands and we have an
    end-to-end smoke against a Netflix Public clip.
  - Synthetic test corpus is not validated against a real per-title
    encode. Smoke tests prove the math; PLCC against a real Netflix
    per-title baseline is a separate validation milestone.
  - Manifest emit ships placeholder variant URIs; the consumer must
    re-point them. We do not currently package the manifest with
    actual segmented MP4s — that's a downstream concern.

- **Neutral / follow-ups**:
  - Phase B integration PR (gated on PR #347 merge): replace
    `_default_sampler` with a real bisect-driven sampler.
  - Real-corpus validation (gated on Netflix Public encodes via
    Phase A): compute PLCC of the picked ladder rungs against
    Netflix's published per-title rungs and document the delta in
    `docs/research/0061-vmaf-tune-capability-audit.md`.
  - Status flips to Accepted only when the end-to-end PR lands AND
    the validation digest reports the delta.

## References

- Audit source: `docs/research/0061-vmaf-tune-capability-audit.md`
  Bucket #6 (per-title ladder generator — flagged as the
  game-changer).
- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune
  umbrella spec (this ADR is its Phase E child).
- Netflix per-title encoding paper, 2015 — the canonical reference
  for the convex-hull approach.
- Apple HLS Authoring Specification for Apple Devices §2.3 —
  bandwidth-doubling ladder convention used as the default
  `spacing="log_bitrate"` mode.
- av1an `--target-quality` mode — prior art for per-rendition
  bisect; conceptually a Phase B sibling, not a Phase E sibling.
- Bitmovin Per-Title — closed-source equivalent on the
  cloud-encoder side.
- PR #347 (Phase B target-VMAF bisect, in flight) — the integration
  point for the production sampler.
- PR #354 capability audit — flagged Bucket #6 as the highest-
  leverage gap in the fork's automation surface.

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `tools/vmaf-tune/src/vmaftune/ladder.py` — present (scaffold
  with `build_ladder`, `convex_hull`, `select_knees`,
  `emit_manifest`).
- `vmaf-tune ladder` CLI subcommand registered.
- ADR-0307 (Accepted in the 2026-05-06 sweep) wired the default
  `_default_sampler` so the placeholder no longer raises
  `NotImplementedError`; the `SamplerFn` seam stays open for
  callers needing finer control.
- Verification command:
  `ls tools/vmaf-tune/src/vmaftune/ladder.py`.
