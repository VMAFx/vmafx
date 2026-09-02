<!-- markdownlint-disable MD060 -->
# Research-0069: Region-of-interest VMAF — option-space digest

- **Status**: digest (informs ADR-0288)
- **Date**: 2026-05-03
- **Relevant ADRs**: ADR-0288 (`vmaf-roi` saliency-weighted scaffold),
  ADR-0286 (`saliency_student_v1`), ADR-0042 (tiny-AI docs rule).

## Question

Given the fork's existing `saliency_student_v1` model (per-pixel RGB
→ [0,1] saliency mask), what is the cheapest path to a usable
"region-of-interest VMAF" surface — one that lets a downstream caller
weight the VMAF score by saliency so content with bad backgrounds but
good faces is not over-penalised?

## Surfaces surveyed

The libvmaf reduction pipeline collapses per-pixel feature responses
into per-frame scalars and then per-clip pooled scalars. The surfaces
where saliency could attach are, in order from "deepest" to "outermost":

1. **Inside the feature kernels** — multiply each pixel's feature
   contribution by `saliency[i,j]` before the per-pixel reduction.
   Touches every feature implementation (CPU, AVX2, AVX-512, NEON,
   CUDA, SYCL, Vulkan).
2. **In the per-frame collector** — between the feature kernel and the
   per-frame scalar, weight the running sum by the saliency mask. Lives
   in `core/src/feature/feature_collector.c`. One file, one path,
   but bit-exactness contract still applies.
3. **In the model JSON's `feature_norm`** — saliency-weight the
   normalised feature vector before the SVR predict call. Adds a new
   per-frame dependency to the model schema.
4. **In the temporal pool** — weight the per-frame VMAF scalars by a
   per-frame saliency aggregate before pooling across frames. Easy,
   but doesn't address spatial saliency.
5. **At the tool level** — score twice (full-frame + saliency-masked
   distorted YUV) and blend the scalars. No libvmaf changes.

## Decision matrix

| # | Surface | Per-pixel correctness | C changes | Bit-exactness risk | Wall-clock | Time-to-ship |
|---|---|---|---|---|---|---|
| 1 | Feature kernels | exact | every backend | very high | 1× | weeks (full GPU-parity matrix) |
| 2 | Feature collector | exact | one file | high | 1× | days (numerical validation needed) |
| 3 | Model JSON `feature_norm` | approximate | minimal | low | 1× | days (schema bump + regen) |
| 4 | Temporal pool | none (spatial signal already lost) | none | none | 1× | hours (but solves wrong problem) |
| 5 | **Tool-level (Option C, chosen)** | approximate (substitution-based) | none | none | 2× | hours |

## Why Option C wins for the first scaffold

- **Zero exposure to the Netflix golden gate**: the libvmaf binary is
  unchanged; the existing CPU bit-exactness contract is preserved
  byte-for-byte.
- **Zero exposure to cross-backend numerical drift**: surfaces 1–3 all
  require the new weighting to match across CPU / CUDA / SYCL / Vulkan,
  which is a multi-week validation matrix. Option C side-steps this.
- **Useful behaviour despite the approximation**: the masked run scores
  the salient region against itself for the masked-out pixels (because
  we substitute the reference's pixels into the distorted YUV outside
  the salient region). Those pixels score as a perfect match, so the
  pooled VMAF for the masked run is dominated by the salient region —
  which is the qualitative behaviour the user asked for.
- **Cheap to dismantle**: if Option A ships later, `vmaf-roi`'s combine
  math becomes a thin wrapper for an already-correct single-call
  invocation. The JSON schema stays compatible; only the implementation
  changes.

## What this approach cannot deliver (the honest list)

- **True per-pixel weighted pooling.** The masked run is a pixel
  *substitution*, not a per-pixel feature *weight*. A salient region
  surrounded by a uniform-grey "perfect match" zone scores differently
  than the same salient region under a *zero-weight* zone — VMAF's
  edge-sensitive features (motion, ADM) react to the boundary between
  the salient region and the substituted background.
- **Mask-boundary artefacts.** A hard threshold creates a sharp step in
  the substituted YUV that VMAF will partially detect as "edge
  distortion". A small fade band at the mask boundary mitigates this
  but does not eliminate it. Documented in the user-facing docs.
- **MOS correlation claims.** ROI-VMAF *might* correlate better with
  subjective quality than uniform VMAF on talking-head / sports
  content. Or it might not. **This PR makes no such claim** —
  validation against a labelled MOS dataset is a separate research
  exercise (and would itself need a corpus of saliency-relevant
  content; the existing Netflix Public + KV + BVI-A/B/C/D corpora are
  not labelled for "where the eyes go").

## What we deliberately don't measure

- ROI-VMAF vs uniform VMAF PLCC / SROCC — out of scope; would need a
  saliency-MOS corpus the fork does not currently have.
- Cross-backend numerical drift of the masked run — out of scope; the
  masked YUV scoring path uses the unmodified `vmaf` binary, so any
  drift it has is the same drift it already has, not a new exposure.
- Per-codec fairness of ROI-VMAF (does it favour codecs that
  preserve faces over codecs that preserve textures?) — interesting
  research question, but for a follow-up PR with a real corpus.

## Open questions for the follow-up Option A ADR

1. Where exactly does the saliency mask attach inside
   `feature_collector.c` — pre-`min_max_norm`? Post-? On VIF only? On
   ADM only?
2. Does the saliency mask itself need to be downsampled to match each
   feature's internal pyramid level, or is bilinear resampling at
   pool time sufficient?
3. What does the cross-backend gate look like — bit-exact with the new
   weighting, or relaxed-tolerance per ADR-0214?
4. Does the model JSON schema need a `roi_weighted` flag, or is the
   weighting a runtime CLI flag with a default of "off"?

These are answered when (and if) Option A is approved. They are listed
here so the next person picking this up has the open-question set
written down.

## Citations

- ADR-0286 (`saliency_student_v1` — fork-trained on DUTS).
- ADR-0237 (`vmaf-tune` — sibling tool layout that `vmaf-roi` copies).
- ADR-0214 (cross-backend numeric tolerance — would govern Option A).
- The user's task brief (paraphrased): pick Option C as the lowest-risk
  scaffold; document Option A as future work; do not claim MOS
  correlation without measurement.
