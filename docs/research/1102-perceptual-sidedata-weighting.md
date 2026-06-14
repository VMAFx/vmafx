<!-- markdownlint-disable MD013 MD060 -->
# Research-1102: Pelorus side-data perceptual weighting of VMAF pooling

- **Status**: Closed (ADR-1118 Accepted)
- **Workstream**: Pelorus <-> vmafx bidirectional integration, workstream B
  (vmafx: read + use side-data)
- **Last updated**: 2026-06-14
- **Related ADR**: [ADR-1118](../adr/1118-perceptual-sidedata-weighting.md)
- **Builds on**: [ADR-1113](../adr/1113-vendor-pelorus-interop-abi.md) /
  [Research-1101](1101-vendor-pelorus-interop-abi.md) (the vendored parser).
- **Source plan**: `.workingdir2/rc/pelorus/PLAN.md` (gitignored planning
  artifact; this digest is the tracked summary of its verified findings).

## Question

Given the vendored Pelorus interop parser, how should vmafx *use* the per-frame
banding/variance maps to make a pooled VMAF reflect perceptual banding salience —
without perturbing the Netflix golden gate, and while degrading gracefully across
an evolving producer (today's `vf_pelorus_deband` emits only a frame-level
placeholder; real per-cell maps are a later workstream)?

## Where pooling actually happens (verified)

The maintainer brief says "spatial pooling". Reading the code
(`core/src/libvmaf.c`) clarified what that means in libvmaf:

- Each frame's VMAF is a single scalar produced by the model from per-frame
  features; true per-pixel spatial aggregation lives deep inside the feature
  extractors (VIF/ADM sum over the image).
- `vmaf_score_pooled` → `vmaf_feature_score_pooled` pools those per-frame scalars
  across a frame range (MEAN / MIN / MAX / HARMONIC_MEAN).

Touching the feature extractors' internal spatial sums would be deeply invasive
and an enormous golden-gate risk. The clean, golden-safe insertion point that
matches the maintainer's API shape (`vmaf_set_perceptual_sidedata` keyed by
`pic_index`; weighting "in the `vmaf_score_pooled` path") is to summarise each
frame's *spatial* banding/variance maps to a per-frame salience and use it as a
**pooling weight**. The spatial maps drive how much each frame contributes —
hence "perceptual spatial-pooling weighting".

## Options weighed

| Option | Verdict |
| --- | --- |
| **Spatial-pooling weighting (weighted MEAN/HARMONIC_MEAN)** | **Chosen** (maintainer decision). Re-weights the score the user already reads; golden-isolated because weight ≡ 1.0 ⇒ byte-identical pooling. |
| Auxiliary feature only | Rejected — golden VMAF untouched, but the pooled number does not reflect banding salience, so the requirement is sidestepped, not met. |
| Both (weight + publish a feature) | Rejected for the RC — doubles surface/doc burden; the auxiliary half is unused by the stated use-case and enlarges the golden-gate blast radius. |

## Golden-gate isolation — how it is guaranteed

The #1 requirement. Two independent gates and a structural choice:

1. **Opt-in (a)** — `vmaf_set_perceptual_weight_enabled`, default OFF; the
   `vf_libvmaf` `perceptual_weight` AVOption, default 0.
2. **Presence (b)** — a frame is only weighted if a *valid* Pelorus blob was
   registered for its index. Frames without one get weight 1.0.
3. **Literal upstream path** — when weighting is inactive
   (`vmaf_perceptual_weight_active()` false), `vmaf_feature_score_pooled` runs
   the *exact* upstream expressions (`sum/pic_cnt`,
   `pic_cnt/i_sum − 1`) in the same order, not a weighted formula that merely
   evaluates to the same value. So the no-side-data path is byte-identical, not
   just numerically close.

The Netflix golden 3 pairs carry no Pelorus side-data, so (b) is false for every
frame and they score bit-exact regardless of (a). This is pinned by
`core/test/test_perceptual_weight.c`: pooling is bit-exact (`==`) without
side-data, with weighting enabled-but-no-side-data, and with side-data-but-
disabled; it shifts in the expected direction only when both gates are open.

## Weight model (verified at the unit-test level)

`salience ∈ [0,1]` = mean per-cell banding risk (uint8 cells → [0,1]) when a
non-empty grid + valid map are present; else the frame-level banding scalars;
variance modulates it within `[0.75, 1.25]` (flat ⇒ more banding-visible).
`weight = 1 + strength · salience` (strength default 1.0). Then
MEAN → `Σ(wᵢ·sᵢ)/Σ(wᵢ)`, HARMONIC_MEAN → `Σwᵢ/Σ(wᵢ/(sᵢ+1)) − 1`. The test pins
the closed form: for scores {60,80,100} with frame 2 at salience 1.0
(weight 2.0), the weighted mean is `(60+80+200)/4 = 90`, exceeding the
unweighted `80`.

## Robustness (R1–R6) — verified

- **R4**: each section read for `min(known_size, dir.size)` bytes via the
  vendored `pel_blob_find_section`; per-cell map reads independently
  bounds-checked against the blob image before dereference.
- **R3**: unknown section bits ignored (the parser only returns requested bits).
- Absent sections / `grid == 0` (today's deband placeholder) degrade to a
  frame-level scalar or to "no salience" (weight 1.0) — `test_grid_zero_*`.
- **R6**: ABI-major mismatch → `-EPROTO`, frame unweighted + logged —
  `test_robustness_foreign_and_bad_abi` (also covers a foreign buffer → -ENOENT).
- NaN/Inf inputs clamped; derived weight always finite and positive; loops
  bounded by the validated grid; odd-dim safe (cell count is `cols*rows`).

## Outcome

Implemented as B1 (C-API) + B2 (weighting in `vmaf_feature_score_pooled`) + B4
(R1–R6 compat) in `libvmaf`, plus B3 (`ffmpeg-patches/0017-*`). Full CPU fast
suite green (101/101), the new golden-isolation test green (7/7),
clang-format + clang-tidy clean. Real per-cell maps depend on
`vf_pelorus_analyze` (plan C) on the Pelorus side; until then the reader runs the
frame-level-scalar degrade path.
