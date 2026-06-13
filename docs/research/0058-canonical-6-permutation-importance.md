<!-- markdownlint-disable MD025 -->
# Research 0058 — canonical-6 permutation importance for `vmaf_tiny_v2`

# Research-0058

- **Status:** Complete
- **Date:** 2026-05-03
- **Author:** Lusoris (Claude Opus 4.7 agent)
- **Companion ADRs:** _none — empirical research only, no decision change_
- **Companion code:** [`scripts/dev/permutation_importance.py`](../../scripts/dev/permutation_importance.py)

## Question

Of the canonical-6 features fed to the shipped `vmaf_tiny_v2.onnx` regressor
(`adm2`, `vif_scale0..3`, `motion2`), which carry the strongest signal toward
the teacher VMAF prediction, and are any near-zero (candidates for removal in
a future v3+)?

## Method

Permutation importance, model-agnostic and training-free:

1. Load `model/tiny/vmaf_tiny_v2.onnx` via onnxruntime CPU EP. The model has
   the StandardScaler baked in as Constant nodes (per
   [`vmaf_tiny_v2.json`](../../model/tiny/vmaf_tiny_v2.json)), so raw feature
   values are fed directly.
2. Sample 5000 rows uniformly at random (seed 20260503) from
   `runs/full_features_4corpus.parquet` (330 499 frames over Netflix Public,
   KoNViD, BVI-A, BVI-B, BVI-C, BVI-D).
3. Compute baseline PLCC of the ONNX prediction vs the teacher `vmaf` column.
4. For each feature column, permute its values (`np.random.permutation`) and
   re-score. Repeat 5 times with seeds `1000..1004`. Report mean ± std.
5. The drop in PLCC is the feature's importance.

Wall time: ~3 seconds on CPU (no GPU needed). Reproducible via the script
above.

## Results

Baseline PLCC: **0.999897** (5000-row sample).

| Rank | Feature      | Baseline PLCC | After permutation | Drop ± std        |
|------|--------------|---------------|-------------------|-------------------|
| 1    | `adm2`       | 0.9999        | 0.5470            | +0.4529 ± 0.0040  |
| 2    | `motion2`    | 0.9999        | 0.6545            | +0.3454 ± 0.0037  |
| 3    | `vif_scale3` | 0.9999        | 0.8950            | +0.1049 ± 0.0007  |
| 4    | `vif_scale2` | 0.9999        | 0.9439            | +0.0560 ± 0.0004  |
| 5    | `vif_scale1` | 0.9999        | 0.9968            | +0.0031 ± 0.0001  |
| 6    | `vif_scale0` | 0.9999        | 0.9981            | +0.0018 ± 0.0001  |

## Findings

- **`adm2` is the dominant signal** (drop −0.453). Permuting it collapses
  PLCC from 0.9999 to 0.55 — the model effectively cannot reconstruct VMAF
  without ADM. This matches Netflix's own ranking in the original VMAF
  paper (ADM2 + VIF + motion).
- **`motion2` is a strong second** (drop −0.345). Temporal energy
  contributes meaningfully despite the scalar-per-frame nature. Worth
  keeping; cannot be substituted by spatial features.
- **`vif_scale3` and `vif_scale2` carry the bulk of the VIF signal**
  (drops −0.105 and −0.056). The coarse VIF scales encode global luminance
  fidelity that ADM (edge-focused) misses.
- **`vif_scale0` and `vif_scale1` are near-zero** (drops +0.0018 and
  +0.0031, both <1 % PLCC each). The fine VIF scales are essentially
  redundant with `vif_scale2/3` in the small-MLP regime — the model has
  learned to weight them ~0. Consistent with classic VIF's known
  high-scale dominance for natural-content distortions.

## Decision implication

A future `vmaf_tiny_v3` (or a tinier `vmaf_nano`) experiment dropping
`vif_scale0` and `vif_scale1` from the input vector should be tractable —
expected PLCC delta on the held-out set is on the order of 0.005, which
may be acceptable for a 4-feature canonical input that halves the VIF
extractor work. **Not making that decision here**; flagging as a follow-up
hypothesis for the canonical-N exploration backlog.

This research digest does **not** mandate any code or model change. The
shipped `vmaf_tiny_v2` keeps its 6-feature input.

## Reproducer

```bash
python3 scripts/dev/permutation_importance.py
```

Produces the table above in ~3 seconds against the shipped ONNX and the
4-corpus parquet in `runs/`.

## ADR-0108 deliverables (research-only PR)

1. **Research digest** — this file.
2. **Decision matrix** — _no alternatives: only-one-method study (permutation
   importance is the standard model-agnostic technique; no training-based
   alternative was within the time-box)._
3. **`AGENTS.md` invariant note** — _no rebase-sensitive invariants._ This
   PR adds one researcher script and one doc; nothing patches upstream
   surfaces.
4. **Reproducer / smoke-test command** — see "Reproducer" above.
5. **`CHANGELOG.md` entry** — opt-out: research-only, no user-visible
   change.
6. **`docs/rebase-notes.md` entry** — _no rebase impact: research-only doc
   ev script under `scripts/dev/`._

## References

- `model/tiny/vmaf_tiny_v2.json` — sidecar with baked-in scaler stats.
- [PR #250](https://github.com/VMAFx/vmafx/pull/250) — `vmaf_tiny_v2`
  ship commit `3999cdab`.
- [docs/research/0046-vmaf-tiny-v3-mlp-medium-evaluation.md](0046-vmaf-tiny-v3-mlp-medium-evaluation.md)
  — prior canonical-6 evaluation in the v3 (mlp_medium) regime.
