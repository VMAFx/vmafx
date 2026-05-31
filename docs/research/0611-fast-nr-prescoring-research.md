<!-- markdownlint-disable MD060 -->
# Research Digest 0611: Fast NR Pre-Scoring

**Scope**: Use the in-tree `nr_metric_v1` ONNX model as a coarse VMAF proxy
during the bisect's early iterations; switch to full FR VMAF only on the
candidate-best CRF. Goal: cut bisect wall-time substantially.
**Retrieved**: 2026-05-19
**Status**: Planning-only; no implementation.

---

## Background

### Why FR VMAF is expensive

Full-reference (FR) VMAF requires decoding both the reference YUV and the
distorted encode, then running the 6 libvmaf feature extractors (ADM, VIF ×4,
motion2) frame by frame. On a 10-second HD shot at 30 fps (300 frames), a
single FR VMAF call takes ~2–5 seconds on CPU even with AVX2 paths. The
Phase B bisect makes 5–8 such calls per shot (log₂(CRF range) ≈ 5 for a
[18,40] window). A title with 500 shots costs ~10,000–20,000 VMAF seconds
of encoding + scoring.

### NR VMAF as a proxy

No-reference (NR) metrics infer perceptual quality from the distorted signal
alone, without the reference YUV. They are inherently faster because:

- No reference decode / YUV copy.
- Lighter feature set (spatial statistics, DCT moments, NIQE-style, etc.).
- Can run on the encoder output without a separate decode step if features
  are extractable from the bitstream.

The fork already ships `nr_metric_v1.onnx` (and INT8 variant) under
`model/tiny/`. ADR-0346 covers the FR-from-NR adapter; the NR model is
already calibrated to predict VMAF-like scores on in-domain content.

### Calibration gap

NR metrics correlate with FR VMAF but are not numerically identical. On
in-domain content (Netflix corpus) the Pearson correlation is typically
0.85–0.92; on out-of-domain content it can drop to 0.70. Using an NR score
directly as a VMAF substitute without calibration would misplace the bisect
window, wasting the savings from fewer FR calls.

A two-stage approach — NR for early bisect elimination, FR for final
candidate confirmation — avoids miscalibration: if the NR score is far
from the target (outside a ±5 VMAF "uncertainty zone"), we can safely
skip the FR call. If NR is within the uncertainty zone, we pay the FR cost.

---

## In-Tree Assets

| Asset | Path | Notes |
|-------|------|-------|
| `nr_metric_v1.onnx` | `model/tiny/nr_metric_v1.onnx` | FP32, ~2 MB |
| `nr_metric_v1.int8.onnx` | `model/tiny/nr_metric_v1.int8.onnx` | INT8 quant, ~0.5 MB |
| `fr_from_nr_adapter.py` | `tools/vmaf-tune/src/vmaftune/fr_from_nr_adapter.py` | FR corpus from NR rows |
| `score_backend.py` | `tools/vmaf-tune/src/vmaftune/score_backend.py` | Backend selection seam |

The `score_backend.py` module is the natural integration point: it already
abstracts the scoring backend. Adding an `NRProxyBackend` that delegates
to `nr_metric_v1` and falls back to full FR when within the uncertainty zone
would be a minimal-diff extension.

---

## Design Options

### Option A: NR-only bisect with FR confirmation on final CRF

Run all bisect iterations using NR. At the end of the bisect, run one FR call
on the winner to confirm the NR prediction was accurate. If the FR score
deviates by more than a threshold (e.g. ±3 VMAF), re-run the last 2 bisect
steps with FR.

**Pros**: Maximum speedup (NR for 6/7 calls, FR for 1–3 calls).
**Cons**: Risk of selecting a wrong CRF if NR is systematically biased on
this content type; final re-run adds back some cost.

### Option B: NR for early elimination, FR at decision boundary (recommended)

Compute the NR score at each bisect midpoint. If `|NR_score - target| > δ_fast`
(e.g. > 8 VMAF), skip FR and proceed the bisect in the NR-implied direction.
If `|NR_score - target| ≤ δ_fast`, pay the FR cost. This keeps FR calls
only for the shots where it matters (close to target).

**Pros**: Correct by construction on boundary calls; speedup is 2–4× on
easy shots (far from target); degrades gracefully to pure FR on hard shots.
**Cons**: Two-model invocation pipeline; requires calibrating `δ_fast`.

### Option C: Conformal-calibrated NR threshold

Use the existing `conformal.py` / `uncertainty.py` infrastructure to produce
a calibrated `(low, high)` interval around the NR prediction. Only substitute
FR when the interval overlaps the target VMAF region. Tighter intervals →
fewer FR calls.

**Pros**: Principled uncertainty quantification; leverages existing conformal
machinery (ADR-0279).
**Cons**: Conformal calibration requires a held-out calibration set for the
NR model; adds 1–2 weeks to implementation vs Option B.

### Option D: Bitstream-feature NR (no decode required)

Extract features directly from the encoder's bitstream (DCT coefficient
statistics, motion vector energy, QP maps) rather than decoding the YUV.
Eliminates the decode step entirely for NR scoring.

**Pros**: Fastest possible; zero decode overhead.
**Cons**: Requires encoder-specific bitstream parsers; not portable across
codecs (H.264 vs AV1 vs HEVC bitstream formats differ); significant
engineering effort.

---

## Recommended Decision Matrix

| Option | Speedup | Correctness risk | Implementation cost |
|--------|---------|-----------------|---------------------|
| A — NR-only + FR confirm | 3–6× | Medium | 2 days |
| B — NR early elim (recommended) | 2–4× | Low | 2–3 days |
| C — conformal NR | 2–4× | Very low | 5–7 days |
| D — bitstream NR | 5–10× | Medium-high | 3–4 weeks |

---

## Calibration Plan (Option B)

1. Run the full Netflix training corpus (`.workingdir2/netflix/` — 9 ref + 70 dis
   YUVs) through both NR and FR VMAF at a grid of CRFs.
2. Fit a monotone calibration curve: `vmaf_fr ≈ f(vmaf_nr)`.
3. Measure residual standard deviation σ. Set `δ_fast = 2σ` (covers >95% of
   in-domain content within the uncertainty zone correctly).
4. Validate on held-out clips. Document in `model/tiny/nr_metric_v1.json` as
   `calibration_threshold`.

---

## Open Questions

1. Should `δ_fast` be content-type-specific (e.g. looser for animation,
   tighter for sports) or global?
2. Is the INT8 variant accurate enough for bisect guidance, or should FP32
   be required?
3. Does the NR model need retraining against the current FR VMAF version
   (`vmaf_v0.6.1` vs `vmaf_v0.6.1neg` etc.)?

---

## References

- `model/tiny/nr_metric_v1.onnx` — in-tree NR model.
- `model/tiny/nr_metric_v1.json` — model card.
- `tools/vmaf-tune/src/vmaftune/fr_from_nr_adapter.py` — FR-from-NR adapter
  (ADR-0346).
- `tools/vmaf-tune/src/vmaftune/bisect.py` — Phase B bisect seam.
- `tools/vmaf-tune/src/vmaftune/score_backend.py` — backend selection.
- `tools/vmaf-tune/src/vmaftune/conformal.py` — conformal prediction infra
  (ADR-0279).
- ADR-0615 — Decision record for fast NR pre-scoring.
