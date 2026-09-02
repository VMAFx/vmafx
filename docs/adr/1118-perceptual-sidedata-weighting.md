<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1118: Pelorus perceptual side-data weights VMAF spatial pooling, golden-isolated and opt-in

- **Status**: Accepted
- **Date**: 2026-06-14
- **Deciders**: Lusoris
- **Tags**: scoring, pooling, pelorus, interop, ffmpeg, golden-gate, fork-local

## Context

The vendored Pelorus interop ABI (ADR-1113) lets vmafx *read* the per-frame
side-data blob that the Pelorus `vf_pelorus_*` pre-encode filters attach to each
distorted AVFrame. That blob carries per-cell banding-risk and local-variance
maps. The integration plan's workstream B asks vmafx to *use* those maps to make
a pooled VMAF track the distortions a deband-aware encoder actually cares about
— regions at high banding risk should count for more.

The hard constraint is the Netflix golden gate. The fork preserves Netflix's
three canonical CPU reference pairs as the numerical-correctness ground truth,
and their hardcoded `assertAlmostEqual` values are a required CI status check
that must never be modified or perturbed. Those pairs carry no Pelorus
side-data. So whatever the reader does, the default scoring path — and the
golden pairs specifically — must remain byte-identical to today.

A second constraint is forward/back-compat with an evolving producer. Today's
`vf_pelorus_deband` emits only a placeholder banding section with an empty grid
(`grid_cols == 0`) and no per-cell maps; the real per-cell `vf_pelorus_analyze`
filter is a later workstream. The reader must therefore degrade gracefully
across the ABI's R1–R6 rules: read `min(known_size, dir.size)` per section,
ignore unknown section bits, tolerate absent sections / an empty grid (fall back
to frame-level scalars), and reject an ABI-major mismatch without crashing.

## Decision

vmafx uses the Pelorus banding/variance maps to **perceptually re-weight VMAF's
spatial pooling**: each frame's per-cell banding-risk map is summarised to a
normalized `[0,1]` salience, modulated by local variance (flat regions make
banding more visible), and turned into a per-frame pooling weight
`w = 1 + strength · salience`. The pooled MEAN and HARMONIC_MEAN become the
*weighted* mean and weighted harmonic mean over per-frame VMAF scores; MIN/MAX
are unaffected (re-weighting cannot reorder extremes). The spatial maps drive
how much each frame contributes — "spatial-pooling weighting".

**Golden-gate isolation is the load-bearing invariant.** The weighting is inert
unless BOTH (a) the operator opts in (`vmaf_set_perceptual_weight_enabled`,
default OFF; the `vf_libvmaf` `perceptual_weight` AVOption, default 0) AND (b) a
valid Pelorus blob was registered for that frame (`vmaf_set_perceptual_sidedata`,
keyed by picture index). When either is false, the per-frame weight is exactly
`1.0` and the pooling code takes a branch whose float operations are identical,
in the same order, to upstream. With all weights `1.0`, the weighted mean equals
`Σsᵢ/N` and the weighted harmonic mean equals `N/Σ(1/(sᵢ+1)) − 1` — but the
implementation runs the *literal* upstream expression on the no-side-data path
rather than a weighted formula that merely evaluates to the same number, so the
result is byte-identical, not just numerically close. The Netflix golden pairs
have no side-data and so score bit-exact, proven by a unit test
(`test_perceptual_weight.c`) that pools with and without a synthetic blob.

New public C-API (`core/include/libvmaf/perceptual_weight.h`):
`vmaf_set_perceptual_weight_enabled`, `vmaf_set_perceptual_weight_strength`, and
`vmaf_set_perceptual_sidedata(VmafContext*, const uint8_t *blob, size_t len,
unsigned pic_index)`. The weight derivation + per-frame store live in
`core/src/feature/perceptual_weight.c`; the pooling hook is in
`vmaf_feature_score_pooled`. The `vf_libvmaf` filter reads the blob off the
distorted frame and drives the API (ffmpeg-patch
`0017-libvmaf-read-pelorus-sidedata.patch`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Spatial-pooling weighting (chosen)** | Directly answers the plan's "regions at high banding risk count more"; a single pooled VMAF reflects the perceptual intent; golden-isolated by construction (weight ≡ 1.0 ⇒ byte-identical) | Changes the meaning of a pooled score when enabled; MIN/MAX are necessarily unaffected; needs careful proof that the no-side-data path is bit-exact | Maintainer decision. It is the only option that re-weights the *score the user already reads*, and the golden gate is protected by the opt-in-AND-present double gate plus a bit-exactness test |
| **Auxiliary feature only** | Golden VMAF is untouched by construction (a separate `pelorus_banding` feature is published alongside, never folded into the model score) | Does not satisfy the plan's intent (the pooled VMAF itself should reflect banding salience); adds a feature consumers must explicitly query; no effect on the headline number | Safe but inert — it sidesteps the requirement instead of meeting it |
| **Both (weight pooling AND publish an auxiliary feature)** | Maximum information; downstream can choose | Doubles the surface and the doc/maintenance burden; the auxiliary half is unused by the stated use-case; larger blast radius for the golden gate | Over-scoped for the RC; the auxiliary feature can be added later without re-litigating this decision |

## Consequences

- **Positive**: with `perceptual_weight=1` and Pelorus side-data present, a
  pooled VMAF up-weights frames whose flat regions are at banding risk, which is
  exactly what a deband-tuning loop wants to optimise against. With the option
  off (default) there is zero behavioural change anywhere, including the golden
  pairs.
- **Negative**: when enabled, a pooled score is no longer a plain mean of
  per-frame VMAF — operators must understand the weighting is active. MIN/MAX
  pooling is deliberately unweighted, which is a documented asymmetry.
- **Neutral / follow-ups**: the real per-cell maps depend on
  `vf_pelorus_analyze` (plan C) landing on the Pelorus side; until then the
  reader runs on the frame-level-scalar degrade path (grid == 0). A future
  ABI-minor bump that appends fields to the banding/variance sections is
  absorbed transparently by the `min(known_size, dir.size)` read (R4).

## Supply-chain impact

- **New dependencies**: none. The reader is CPU-only, dependency-free C compiled
  into the existing `libvmaf` target; it consumes only the already-vendored
  Pelorus interop parser (ADR-1113). No Vulkan, no GPU, no new runtime/build/test
  dependency.
- **Removed dependencies**: none.
- **Build-time fetches**: none.
- **CVE surface delta**: negligible — the new code is a bounds-checked
  flat-buffer reader (every per-cell map read is validated against the blob
  image before dereference) plus arithmetic on already-computed scores. No
  network/IO surface. NaN/Inf inputs are clamped; the derived weight is always
  finite and positive.

## References

- Integration plan: `.workingdir2/rc/pelorus/PLAN.md` — workstream B (B1 C-API
  `vmaf_set_perceptual_sidedata`, B2 weighting in the `vmaf_score_pooled` path,
  B3 `vf_libvmaf` reads the blob + `perceptual_weight` AVOption, B4 R1–R6
  compat) and the "P-DECISION RESOLVED (2026-06-14)" section recording the
  maintainer decision: "reader weighting = SPATIAL-POOLING WEIGHT … GOLDEN-GATE
  ISOLATION (mandatory): the weighting activates ONLY when (a) the vf_libvmaf
  `perceptual_weight` option is set AND (b) pelorus side-data is present on the
  frame."
- ADR-1113 — vendored Pelorus interop ABI (the parser this reader consumes).
- Pelorus ADR-0103 — `interop.h` ABI freeze and the R1–R6 forward/back-compat
  rules this reader honours.
- Golden gate: `CLAUDE.md` §8 (Netflix golden-data gate, do not modify),
  `python/test/` `assertAlmostEqual` values.
