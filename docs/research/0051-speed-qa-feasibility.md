<!-- markdownlint-disable MD060 -->
# Research-0051: SpEED-QA full-reference metric — feasibility on the lusoris fork

- **Status**: Active (Proposed ADR pending sign-off)
- **Workstream**: full-reference metric inventory; companion to
  [`research-0010`](0010-speed-netflix-upstream-direction.md)
- **Last updated**: 2026-05-03
- **Related ADR**: [ADR-0253](../adr/0253-speed-qa-extractor.md) (Proposed)

## Question

Should the fork stand up SpEED-QA as a first-class full-reference (FR)
quality metric on top of the SpEED feature extractors that PR #213
already imported, or is the current FR set (`vmaf`, `ssim`, `ms_ssim`,
`vif`, `adm`, `psnr`, `psnr_hvs`, `ciede`, `ssimulacra2`) sufficient?

The user's 2026-04-21 deep-research memory note flagged SpEED as a
queued track. Research-0010 closed the *Netflix-direction* question
(no SpEED-driven Netflix VMAF-v3 is imminent). This digest closes the
companion *fork-direction* question on the QA reduction itself.

## SpEED-QA — algorithm summary

SpEED ("Spatial Efficient Entropic Differencing") is the spatial-only
sibling of the temporal SpEED-QA family proposed by Bampis, Gupta,
Soundararajan and Bovik in *"SpEED-QA: Spatial Efficient Entropic
Differencing for Image and Video Quality"* (IEEE Signal Processing
Letters, 24(9), 1333–1337, 2017,
[DOI 10.1109/LSP.2017.2726542](https://ieeexplore.ieee.org/document/7979533/)).

Conceptually it is a divisive-normalisation entropy estimator, in the
same lineage as VIF / RR-VIF but built around block-mean-removed
local energy and a Gaussian Scale Mixture (GSM) prior:

1. Mean-remove each frame in spatial blocks (`b × b`, default `b=5`).
2. Estimate per-block variance (the `σ²` field).
3. Form `log(1 + αₚ × σ²_ref / σ²_dist)` style entropy ratios, then
   pool to a per-frame scalar.
4. (Multi-scale variant, "M-SpEED-QA") repeats at 2–3 dyadic scales.

Headline reported numbers — LIVE-VQA / CSIQ / EPFL-PoliMI / LIVE
Mobile — SROCC parity with VIF and ST-RRED at ~10–40× the throughput
because the algorithm avoids the full steerable-pyramid /
divisive-normalisation pipeline that ST-RRED uses. The 2017 paper is
**not** a learning-based model; the score is closed-form, no trainer
required.

## Upstream Netflix status

The Netflix tree was scanned 2026-05-03 against `upstream/master` and
the historical `upstream/speed_ported` branch:

- **Source code** — `core/src/feature/speed.c` (1,567 LOC,
  scalar-only) was ported by upstream commit
  [`d3647c73`](https://github.com/Netflix/vmaf/commit/d3647c73)
  ("feature/speed: port speed_chroma and speed_temporal extractors")
  and merged by [`9dac0a59`](https://github.com/Netflix/vmaf/commit/9dac0a59)
  ("libvmaf/feature: update alias map for cambi/speed"). The fork
  carries both via PR #213 (commit `32f27578`).
- **Registered extractors** — `speed_chroma` (Y / U / V / UV scores)
  and `speed_temporal` (single score over a small cyclic buffer).
  Both gated behind `-Denable_float=true` /
  `VMAF_FLOAT_FEATURES=1`. Verified by `grep -nE
  "vmaf_fex_speed" core/src/feature/feature_extractor.c`.
- **No SpEED-QA reduction** — neither upstream nor the fork ships a
  `speed_qa` extractor that turns the per-block field into a single
  full-frame quality score in the form the 2017 paper describes.
  `speed_chroma` is a *research-stage* per-plane fidelity probe and
  `speed_temporal` is a flicker / judder probe; neither is a drop-in
  SpEED-QA score.
- **No model JSON consumes any SpEED feature.** Verified across the
  9 model JSONs under `model/` and the `model/other_models/` archive
  on both `upstream/master` and `master`. The task brief referenced a
  hypothetical `model/speed_4_v0.6.0.json`; **no such file exists**
  anywhere in the upstream tree, the fork tree, or any open Netflix
  PR. The brief's assumption was incorrect — there is no
  pre-existing Netflix SpEED model to mirror.

## What new signal would SpEED-QA add?

The fork's current FR inventory:

| Family       | Extractor          | What it captures                                  |
|--------------|--------------------|---------------------------------------------------|
| Pixel-error  | `psnr`             | MSE in pixel space                                |
| Pixel-error  | `psnr_hvs`         | DCT-weighted MSE with CSF                         |
| Colour       | `ciede`            | CIEDE2000 ΔE                                      |
| Structural   | `ssim` / `ms_ssim` | Luminance / contrast / structure                  |
| Information  | `vif`              | Mutual-info under GSM prior, multi-scale          |
| Information  | `adm`              | Detail-loss + additive-impairment under GSM       |
| Perceptual   | `ssimulacra2`      | XYB-domain MS-SSIM + edge / artefact heuristics   |
| Fusion       | `vmaf`             | Linear / SVR fusion of `vif`, `adm`, motion       |

SpEED-QA's GSM-entropy backbone overlaps **substantially** with VIF
(both reduce divisively-normalised mutual information under a GSM
prior). The 2017 paper's headline contribution is *speed*, not a new
perceptual axis — SROCC parity with VIF and ST-RRED at lower compute.
Two consequences for this fork specifically:

1. **Perceptual coverage gap.** SpEED-QA does not unlock a quality
   axis the existing inventory misses. The remaining gaps in the
   inventory are *high-frequency masking* (already addressed by
   ssimulacra2 + psnr_hvs), *temporal masking* (out of scope for a
   spatial QA), and *learned saliency* (already addressed by the
   tiny-AI track in `ai/`).
2. **Performance argument is moot for the fork.** VIF on the fork
   has AVX2 + AVX-512 + NEON + CUDA + SYCL paths and benches at
   8–17× scalar SpEED on equivalent resolutions per the
   `testdata/netflix_benchmark_results.json` snapshot. SpEED-QA's
   "10–40× faster than VIF" advantage is measured against a
   reference scalar VIF; it inverts on the fork's optimised stack.

The one signal SpEED-QA would add over VIF is the
**block-mean-removed entropy field** that survives heavy chroma
distortion better than VIF's luma-only pipeline. The fork already
exposes that field via the `speed_chroma_uv_score` extractor; no QA
reduction is required to consume it.

## Implementation cost estimate

Three plausible scopes:

| Scope         | LOC (.c/.h)              | Extractors  | GPU / SIMD readiness                                                                 | New tests          |
|---------------|--------------------------|-------------|--------------------------------------------------------------------------------------|--------------------|
| GO (full)     | +1 800 — 2 400           | `speed_qa`, `speed_qa_ms` | scalar from day-0; AVX2 / NEON via `simd_dx.h`; CUDA / SYCL via `kernel_template`; Vulkan deferred (T7 backlog) | ~250 LOC unit + 1 cross-backend snapshot |
| SCAFFOLD      | +120 — 200               | none new (manifest only)  | n/a                                                                                  | none               |
| DEFER         | 0                        | none        | n/a                                                                                  | none               |

The "GO" SIMD/GPU coverage is achievable because the SpEED hot loops
are block-pooled variance + `log1p` reductions — exactly the shape the
fork's `kernel_template` (ADR-0246) and `simd_dx.h` (ADR-0140) were
designed for. The `iqa_convolve_avx2` (ADR-0011) edge-mirror handler
is reusable. Estimate: 2–3 weeks for one engineer to land scalar +
AVX2 + CUDA at numeric parity, plus an `--feature speed_qa` CLI doc
update.

The "SCAFFOLD" scope is one model-manifest file ("if Netflix or a
third party publishes a SpEED-driven model JSON, the fork is wired to
load it") plus a doc note in `docs/metrics/features.md`. No kernel
work; no new public C-API surface.

## Decision matrix

| Option                  | Pros                                                                                                                                              | Cons                                                                                                                                                  | Why / why not                                                                                                                                                                                       |
|-------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **GO** (full impl)      | Closes a "completeness" story — SpEED becomes a first-class FR option in the CLI. Multi-scale variant adds a *small* differentiator vs VIF.       | 2–3 weeks of engineering for a metric that overlaps VIF substantially. No corpus / model on the fork uses the score. Adds maintenance surface forever. | The opportunity cost is high: the same engineering window funds either FUNQUE+ (research-0010 §4 — the live efficient-VMAF thread) or the Vulkan-coverage push (T7 backlog), both higher leverage. |
| **SCAFFOLD-ONLY**       | Costs <1 day. Catches the "Netflix ships a SpEED model JSON tomorrow" tail risk cheaply. Documents the existing extractors as the fork's stance.  | Rewards a hypothetical that has no current evidence (research-0010 §3). Adds a half-finished story that some reader will mis-read as "SpEED is shipped". | Marginal value over **DEFER** because the existing `speed_chroma` / `speed_temporal` extractors are already documented and usable. The "model registry entry pointing to upstream binary" the task brief contemplated has no upstream binary to point at. |
| **DEFER** (recommended) | Zero engineering cost. Preserves engineering bandwidth for FUNQUE+, Vulkan coverage, tiny-AI tracks. Status-quo `speed_chroma` / `speed_temporal` remain available as research-stage features with no behavioural change. | Loses the "we have SpEED-QA" marketing line if a competitor or downstream user asks. (Mitigation: this digest plus the existing `docs/metrics/features.md` SpEED section already document the position.) | The signal balance from research-0010 (no Netflix landing imminent), the FR-coverage analysis (SpEED-QA overlaps VIF), and the engineering-bandwidth math all point the same direction. |

## Recommendation

**DEFER.** Status-quo: keep `speed_chroma` and `speed_temporal` as
research-stage extractors gated behind `-Denable_float=true`, do not
add a `speed_qa` reduction, do not add a SpEED model registry entry.
Re-open the question if any of three triggers fire:

1. **Netflix lands a model JSON that consumes SpEED features.**
   Trigger: a `model/*.json` on `upstream/master` with at least one
   `Speed_*_feature_*` reference. Watch via `/sync-upstream`.
2. **A user / customer requests SpEED-QA explicitly** (not "a SpEED
   metric" generically — the existing extractors satisfy that).
3. **FUNQUE+ / pVMAF / a successor explicitly cites SpEED-QA as a
   building block** that the fork's tiny-AI or fusion track wants to
   ingest. Trigger: a research-NNNN or ADR-NNNN that names SpEED-QA
   as a load-bearing input.

Until then the cost-benefit is unfavourable: SpEED-QA does not
expand the fork's perceptual coverage, the "speed" headline is
inverted by the fork's optimised VIF, and there is no model on the
fork's roadmap (Netflix or otherwise) that demands the reduction.

## Alternatives explored

- **GO.** Full implementation — see the cost-estimate table.
  Rejected: 2–3 weeks for a VIF-overlap metric with no consumer.
  Engineering bandwidth funds higher-leverage tracks (FUNQUE+,
  Vulkan coverage, tiny-AI v3 / v4 evaluation per
  research-0048 / -0050).
- **SCAFFOLD-ONLY.** Add a model-registry stub, no kernel.
  Rejected: the registry stub would point at a non-existent
  upstream binary (`model/speed_4_v0.6.0.json` does not exist —
  this digest's primary correction to the task brief). Documents a
  half-finished story.
- **Wait-and-port** (research-0010's residual position). Same as
  DEFER but without recording the decision. Rejected: ADR-0028
  "every non-trivial scope decision gets an ADR" — silent defer
  forces the next session to re-derive the conclusion.

## Open questions

- **Should SpEED-QA reduction be folded into the FUNQUE+ port if /
  when that lands?** FUNQUE+ uses entropy-pooling primitives that
  share kernels with SpEED-QA; a single port could cover both.
  Capture in the FUNQUE+ research digest if it opens.
- **Should `speed_chroma` / `speed_temporal` graduate from
  research-stage to core?** Independent of SpEED-QA. Currently
  blocked by (a) no `VMAF_FLOAT_FEATURES=1` default-on track, and
  (b) no consuming model. Re-evaluate when either lands.

## Reproducer / smoke-test

Verify the SpEED state on the fork at digest time:

```bash
# Confirm extractors registered:
grep -nE "vmaf_fex_speed" core/src/feature/feature_extractor.c
grep -nE "speed_chroma|speed_temporal" core/src/feature/alias.c

# Confirm no model consumes SpEED features:
grep -lr "Speed_chroma_feature\|Speed_temporal_feature" model/ || echo "no consumer (expected)"

# Confirm the assumed-but-missing upstream binary really is missing:
git fetch upstream
git ls-tree -r upstream/master | grep -iE "speed_4|speed.*\.json" || echo "no speed_4_v0.6.0.json (expected)"
```

## Related

- ADRs: [ADR-0253](../adr/0253-speed-qa-extractor.md) (Proposed —
  this digest's companion).
- Research: [`research-0010`](0010-speed-netflix-upstream-direction.md)
  (Netflix direction; closed).
- PRs: fork PR #213 (SpEED port), upstream PRs #1488 / #1391 / #1361
  (context per research-0010).
- Memory:
  `~/.claude/projects/-home-kilian-dev-vmaf/memory/project_deep_research_netflix_upstream_models.md`
  — closing the SpEED leg of the queued track.
