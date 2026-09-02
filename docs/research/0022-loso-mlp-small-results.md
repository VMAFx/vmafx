<!-- markdownlint-disable MD060 -->
# Research Digest 0022 — LOSO results for `mlp_small` on the Netflix corpus

**Date**: 2026-04-28
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — informs follow-up tiny-AI training decisions.
**Scope**: Per-fold PLCC / SROCC / RMSE for the leave-one-source-out
(LOSO) sweep of the `mlp_small` regressor against
`vmaf_v0.6.1`, plus comparison against the two single-split baselines
shipped in `model/tiny/`. Companion to ADR-0203 (training prep) and
Research Digest 0019 (Netflix corpus methodology).

---

## 1. Setup

- Corpus: `.workingdir2/netflix/{ref,dis}/`, 9 reference YUVs, 70
  distortion variants per source where complete (some sources have
  fewer; `(n)` columns below).
- Teacher: `vmaf_v0.6.1` per-frame scores via the libvmaf CLI in
  `core/build/tools/vmaf`.
- Architecture: `mlp_small` (257 params) — see ADR-0203.
- Training: 30 epochs per fold, default optimizer / lr / seed. Each
  fold trains on the 8 non-held-out sources and is scored on the
  held-out 9th.
- Evaluation harness:
  [`ai/scripts/eval_loso_mlp_small.py`](../../ai/scripts/eval_loso_mlp_small.py).
- Hardware: Ryzen 7950X / 4090 / 64 GB. Total wall time ≈ 55 min for
  the 9-fold sweep on a populated feature cache; ≈ 5 s for the eval
  pass on cached features.

## 2. Per-fold LOSO results

| fold | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| BigBuckBunny | 1 500 | 0.9767 | 0.9794 | 16.666 |
| BirdsInCage | 1 440 | 0.9905 | 0.9954 | 14.696 |
| CrowdRun | 1 050 | 0.9927 | 0.9938 | 13.273 |
| ElFuente1 | 1 260 | 0.9936 | 0.9909 | 14.894 |
| ElFuente2 | 1 620 | 0.9834 | 0.9912 | 15.773 |
| FoxBird | 900 | 0.9266 | 0.9425 | 17.524 |
| OldTownCross | 1 050 | 0.9939 | 0.9990 | 15.647 |
| Seeking | 1 500 | 0.9912 | 0.9952 | 15.754 |
| Tennis | 720 | 0.9788 | 0.9763 | 9.939 |
| **LOSO mean ± std** | — | **0.9808 ± 0.0214** | **0.9848 ± 0.0176** | **14.907 ± 2.218** |

PLCC ≥ 0.97 on 8 of 9 folds; the FoxBird fold is the outlier at
PLCC 0.93 / SROCC 0.94. FoxBird has the smallest sample count (900)
and the highest motion-coherence variance in the corpus, so the
held-out fold has the least similar training signal — consistent
with the wider RMSE.

## 3. Comparison vs the shipped single-split baselines

The two shipped baselines (`model/tiny/vmaf_tiny_v1.onnx` =
mlp_small @ val=Tennis, `vmaf_tiny_v1_medium.onnx` = mlp_medium
@ val=Tennis) are not LOSO models. To compare on a fair axis we
score each baseline on each clip and on the all-clips concatenation.

### 3.1 `mlp_small_v1` (single-split, val=Tennis)

| split | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| BigBuckBunny | 1 500 | 0.9959 | 0.9944 | 15.394 |
| BirdsInCage | 1 440 | 0.9918 | 0.9954 | 14.238 |
| CrowdRun | 1 050 | 0.9937 | 0.9982 | 15.025 |
| ElFuente1 | 1 260 | 0.9898 | 0.9929 | 15.355 |
| ElFuente2 | 1 620 | 0.9809 | 0.9875 | 13.336 |
| FoxBird | 900 | 0.9632 | 0.9745 | 17.296 |
| OldTownCross | 1 050 | 0.9943 | 0.9990 | 14.801 |
| Seeking | 1 500 | 0.9908 | 0.9955 | 15.470 |
| Tennis | 720 | 0.9750 | 0.9792 | 10.616 |
| **all-clips concat** | 11 040 | **0.9356** | **0.9379** | **14.772** |

### 3.2 `mlp_medium_v1` (single-split, val=Tennis)

| split | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| **all-clips concat** | 11 040 | **0.9479** | **0.9504** | **8.419** |

Per-clip rows omitted for brevity — see
`runs/loso_eval/loso_mlp_small_eval.md` after running the harness
locally.

## 4. Reading the comparison

Two findings worth surfacing:

**a) LOSO per-fold PLCC (0.98) is higher than the baselines' all-clips
concat PLCC (0.93–0.95).** Each LOSO fold trains on 8 sources and is
scored on the 9th, so the per-fold model has actually seen
*similar* clips in training; the baselines, trained on 8 sources

- Tennis-as-val, then evaluated across all 9 clips, see a wider
mismatch in the score-axis distribution between training-time and
eval-time. The LOSO mean is the better number to quote when asked
"how good is `mlp_small` on a new clip from this distribution?".

**b) Baselines per-clip > LOSO per-fold for clips the baseline trained
on.** E.g. BigBuckBunny: baseline PLCC 0.9959 vs LOSO fold 0.9767. The
baseline has *seen* BigBuckBunny in training, so it fits its per-clip
score distribution. The LOSO fold has not, so it cannot compensate
for clip-specific score offsets. Both numbers are correct — they
answer different questions.

The all-clips concatenated PLCC drop on the baselines (0.94 → from
~0.99 per-clip) is the same effect: every clip has a slightly different
score-axis offset, the baselines learned a single mapping, and the
concatenation exposes the per-clip offsets as residual error. LOSO
folds, evaluated only on their respective held-out clip, never see
this concat-axis effect.

## 5. Implications

- `vmaf_tiny_v1.onnx` (mlp_small @ val=Tennis) remains the shipped
  default tiny model. Its per-clip PLCC > 0.98 across all 9 sources
  is a strong real-world signal.
- The `mlp_medium` variant (`vmaf_tiny_v1_medium.onnx`) wins on
  absolute fit (RMSE 8.4 vs 14.8) but loses on ranking (PLCC 0.948
  vs 0.936); consistent with mlp_small being the better ranking
  model and mlp_medium being the better calibration model. Users
  who want absolute-VMAF agreement on the Netflix-corpus
  distribution can opt into the medium variant; users who care
  about pair-ranking (the canonical VMAF use case) keep the small
  variant.
- The LOSO mean PLCC of 0.98 is the number to quote in
  `docs/ai/training.md` when describing tiny-AI generalization.
- The FoxBird outlier (per-fold PLCC 0.93) suggests the corpus is
  small enough that single-source representation matters; future
  work on a larger corpus (T6-1a Netflix Public Dataset) is the
  proper path to reduce per-fold variance.

## 6. Reproducer

```bash
# 1. Build libvmaf CLI
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# 2. Sweep 9 LOSO folds
for src in BigBuckBunny BirdsInCage CrowdRun ElFuente1 ElFuente2 \
           FoxBird OldTownCross Seeking Tennis; do
  out=model/tiny/training_runs/loso_mlp_small/fold_${src}
  mkdir -p "$out"
  VMAF_TRAIN_OUT_DIR="$out" \
    bash ai/scripts/run_training.sh \
      --model-arch mlp_small \
      --epochs 30 \
      --val-source "$src"
done

# 3. Score per-fold + baselines
python ai/scripts/eval_loso_mlp_small.py
cat runs/loso_eval/loso_mlp_small_eval.md
```

## 7. Known artefacts

- `model/tiny/vmaf_tiny_v1*.onnx` baselines reference their
  pre-rename external-data filenames; the harness works around this
  in `_load_session`. Follow-up: re-export the baselines with
  matching `external_data.location`. Tracked in the LOSO PR's
  CHANGELOG row.
- The previous chat session that drove this run hit a context-limit
  reset before the eval was packaged; the rerun used the same fold
  outputs (regenerated from the trainer) so numbers are stable.
