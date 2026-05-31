<!-- markdownlint-disable MD013 MD060 -->
# Research Digest 0612: VMAF NEG Integration

**Scope**: Integrate the VMAF NEG ("No Enhancement Gain") model variants into
`vmaf-tune` scoring and bisect paths; expose a `--neg` flag; document the
trade-offs vs standard VMAF for codec comparison use cases.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## What Is VMAF NEG?

Netflix's Tech Blog "Toward a Better Quality Metric for the Video Community"
(netflixtechblog.com; SSL retrieval failed 2026-05-19) describes VMAF NEG
as follows (paraphrased from the upstream models documentation, retrieved
via GitHub API 2026-05-19):

> "One unique feature about VMAF that differentiates it from traditional
> metrics such as PSNR and SSIM is that VMAF can capture the visual gain
> from image enhancement operations, which aim to improve the subjective
> quality perceived by viewers. However, in codec evaluation, it is often
> desirable to measure the gain achievable from compression without taking
> into account the gain from image enhancement during pre-processing."

NEG mode disables the enhancement-gain component so that sharpening,
denoising, and other in-loop post-processing operations do not inflate
the VMAF score. This is essential for fair codec comparison: encoder A
could achieve a higher standard VMAF score by sharpening aggressively
rather than by preserving more detail.

### Technical mechanism (SVM model level)

Standard VMAF uses an SVM with features including `adm2` (detail preservation)
and `vif_scale*` (information fidelity). NEG re-trains the SVM on the same
feature set but on a modified subjective dataset where enhancement-boosted
distortions are explicitly penalised. The constraint `out_gte_in: true` in
the score transform is relaxed so the output can go below the input quality
when enhancement is detected.

---

## Model File Inventory

The following NEG model files are already present in `model/`:

| File | Format | Notes |
|------|--------|-------|
| `model/vmaf_v0.6.1neg.json` | JSON SVM | 1080p NEG, the primary file |
| `model/vmaf_float_v0.6.1neg.json` | JSON SVM | float precision |
| `model/vmaf_float_v0.6.1neg.pkl` | Pickle | legacy Python serialisation |
| `model/vmaf_float_v0.6.1neg.pkl.model` | libsvm model | raw libsvm weights |
| `model/vmaf_4k_v0.6.1neg.json` | JSON SVM | 4K variant |

**Conclusion**: The NEG SVM model files are fully present upstream and in the
fork. No training pipeline is required. The gap is integration into
`vmaf-tune`'s CLI and scoring paths.

---

## Current Fork State

| Component | Status |
|-----------|--------|
| NEG model files | Present in `model/` |
| `vmaf` CLI `--model` flag | Supports any `.json` path |
| `vmaf-tune score` backend | Hardcoded to `vmaf_v0.6.1` |
| `bisect.py` model selection | No NEG option exposed |
| `per_shot.py` model selection | Inherits from `bisect.py` |
| Documentation of NEG | None in `docs/` |

---

## Design Options

### Option A: CLI `--neg` flag maps to `vmaf_v0.6.1neg.json`

Add `--neg` to `vmaf-tune score`, `vmaf-tune bisect`, `vmaf-tune per-shot`,
`vmaf-tune compare`. When set, replaces the default model path with
`model/vmaf_v0.6.1neg.json`. No new code paths; purely parameter plumbing.

**Pros**: Minimal implementation; reuses existing `--model` path in libvmaf.
**Cons**: Does not expose 4K or float variants; requires user to know when
NEG is appropriate.

### Option B: `--model-variant` enum: `default | neg | float | float-neg | 4k | 4k-neg`

A structured enum rather than a raw flag. The model registry maps variants
to file paths. Extensible as new models arrive.

**Pros**: Cleaner API; model registry decouples CLI from file paths; compatible
with the existing `model/tiny/registry.json` pattern.
**Cons**: More API surface to maintain; variant enumeration may not be
exhaustive as upstream adds models.

### Option C: Expose raw `--model` path in vmaf-tune (bypass registry)

Pass through a `--model /path/to/any.json` flag to the libvmaf CLI directly.
Users who want NEG specify `--model model/vmaf_v0.6.1neg.json`.

**Pros**: Zero model-specific code; maximum flexibility.
**Cons**: Poor UX; users must know exact file path; no guard against
unsupported model formats; no documentation hook.

### Option D: Per-content-type model auto-selection

The content classifier (Item 6) tags content; a routing table maps
`animation` → NEG (because animation encoders sharpen heavily) vs
`live-action` → standard VMAF. Auto-select without user flag.

**Pros**: Fully automated; eliminates user configuration error.
**Cons**: Requires Item 6 (content classifier) to be implemented first;
auto-selection may surprise users who expect deterministic scores.

---

## Recommended Decision Matrix

| Option | UX | Implementation cost | Dependencies |
|--------|-----|---------------------|--------------|
| A — `--neg` flag (recommended near-term) | Simple | 0.5 day | None |
| B — `--model-variant` enum | Good | 2 days | None |
| C — raw `--model` path | Poor | 0 days | None |
| D — auto-selection | Excellent | 5 days | Item 6 |

Recommendation: ship Option A immediately (it is the smallest-win item on the
roadmap — model files exist, just need wiring). Plan Option D as a follow-on
when Item 6 lands.

---

## When to Use NEG

NEG is correct for:

- Codec A vs codec B comparison (e.g. H.264 vs AV1) when both may use
  in-loop sharpening.
- Pre-processing pipeline evaluation (does denoising actually help?).

NEG is incorrect for:

- Production quality monitoring (end viewer perceives the sharpened output).
- Per-shot CRF selection aimed at maximising viewer QoE (use standard VMAF).

This distinction must be documented in `docs/metrics/vmaf-neg.md`.

---

## Training Pipeline (if upstream NEG model is ever insufficient)

The upstream NEG model was trained on Netflix's private subjective dataset.
If the fork needs to retrain on a different distribution (e.g. a specific
content type where NEG over-penalises legitimate encoder sharpening), the
training pipeline would be:

1. Collect (ref, dis) pairs where enhancement is labelled.
2. Run `python/vmaf/` training harness with NEG constraint active.
3. Validate on held-out set with PLCC / SROCC / RMSE gates.
4. Register new model via `/add-model`.

This is unlikely to be necessary given the existing upstream models.

---

## References

- Netflix VMAF models doc (GitHub API retrieval 2026-05-19):
  `repos/Netflix/vmaf/contents/resource/doc/models.md`.
- Netflix Tech Blog "Toward a Better Quality Metric for the Video Community"
  (URL: netflixtechblog.com/toward-a-better-quality-metric-…; SSL failed
  2026-05-19).
- `model/vmaf_v0.6.1neg.json` — primary NEG SVM model (in-tree).
- `model/vmaf_4k_v0.6.1neg.json` — 4K NEG variant (in-tree).
- ADR-0616 — Decision record for VMAF NEG integration.
