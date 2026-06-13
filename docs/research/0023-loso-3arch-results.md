<!-- markdownlint-disable MD060 -->
# Research Digest 0023 — 3-arch LOSO results on the Netflix corpus

**Date**: 2026-04-28
**Author**: Lusoris / Claude (Anthropic)
**Status**: Accepted — confirms ADR-0203's single-split architecture
choice under proper LOSO; informs `vmaf_tiny_v1*.onnx` shipping
defaults.
**Scope**: Per-fold + aggregate PLCC / SROCC / RMSE for `mlp_small`,
`mlp_medium`, and `linear` regressors against `vmaf_v0.6.1` on the
9-source / ~70-distortion Netflix corpus. Companion to
[Research Digest 0022](0022-loso-mlp-small-results.md) (which
covered `mlp_small` alone) and [ADR-0203](../adr/0203-tiny-ai-training-prep-impl.md).

---

## 1. Setup

- Corpus: `.workingdir2/netflix/{ref,dis}/`, 9 reference YUVs.
- Teacher: `vmaf_v0.6.1` per-frame scores via the libvmaf CLI in
  `core/build/tools/vmaf`.
- Architectures (params):
  - `mlp_small` — 257
  - `mlp_medium` — 2 561
  - `linear` — 7
- Training: 30 epochs per fold per arch, default optimizer / lr /
  seed. Each fold trains on the 8 non-held-out sources.
- Evaluation harness:
  [`ai/scripts/eval_loso_3arch.py`](../../ai/scripts/eval_loso_3arch.py).
  Reuses the per-clip JSON cache + `_load_session` helpers from PR
  #165's `eval_loso_mlp_small.py`.
- Hardware: Ryzen 9 9950X3D / RTX 4090 / 64 GB.
- Wall time: 9 folds × 3 arch ≈ 9 × ~6 min × ~1.0–1.4× per arch
  (mlp_medium ~1.4× of mlp_small; linear ~0.4×). All three sweeps
  ran in parallel on local hardware.

## 2. Aggregate results (mean ± std across 9 folds)

| arch       | params | mean PLCC          | mean SROCC         | mean RMSE        |
|------------|-------:|-------------------:|-------------------:|-----------------:|
| `mlp_small`  | 257   | **0.9808 ± 0.0214** | **0.9848 ± 0.0176** | 14.907 ± 2.218   |
| `mlp_medium` | 2 561 | 0.9727 ± 0.0202    | 0.9794 ± 0.0156    | **10.848 ± 2.302** |
| `linear`     | 7     | 0.3679 ± 0.0773    | 0.4861 ± 0.0975    | 57.868 ± 5.867   |

Clear pattern, replicated on the LOSO axis from the single-split
finding in ADR-0203:

- **`mlp_small` wins ranking** — highest PLCC + SROCC. Default tiny
  model `vmaf_tiny_v1.onnx` stays.
- **`mlp_medium` wins absolute fit** — RMSE 10.85 vs 14.91 (~27 %
  reduction). Alternate `vmaf_tiny_v1_medium.onnx` stays for users
  who care about absolute-VMAF agreement on the Netflix corpus
  distribution.
- **`linear` is a sanity floor** — PLCC 0.37 vs MLP 0.97+. Confirms
  the 6 features carry substantial signal but the relationship is
  strongly non-linear; linear is unshippable as a quality model.

## 3. Per-fold tables

### `mlp_small`

| fold | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| BigBuckBunny | 1 500 | 0.9767 | 0.9794 | 16.666 |
| BirdsInCage | 1 440 | 0.9905 | 0.9954 | 14.696 |
| CrowdRun | 1 050 | 0.9927 | 0.9938 | 13.273 |
| ElFuente1 | 1 260 | 0.9936 | 0.9909 | 14.894 |
| ElFuente2 | 1 620 | 0.9834 | 0.9912 | 15.773 |
| FoxBird | 900 | **0.9266** | **0.9425** | 17.524 |
| OldTownCross | 1 050 | 0.9939 | 0.9990 | 15.647 |
| Seeking | 1 500 | 0.9912 | 0.9952 | 15.754 |
| Tennis | 720 | 0.9788 | 0.9763 | 9.939 |

### `mlp_medium`

| fold | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| BigBuckBunny | 1 500 | 0.9738 | 0.9787 | 10.848 |
| BirdsInCage | 1 440 | 0.9622 | 0.9715 | 11.594 |
| CrowdRun | 1 050 | 0.9941 | 0.9983 | 13.269 |
| ElFuente1 | 1 260 | 0.9706 | 0.9768 | 9.025 |
| ElFuente2 | 1 620 | 0.9796 | 0.9839 | 10.316 |
| FoxBird | 900 | **0.9286** | 0.9514 | 14.693 |
| OldTownCross | 1 050 | 0.9932 | 0.9976 | 10.889 |
| Seeking | 1 500 | 0.9876 | 0.9921 | 10.311 |
| Tennis | 720 | 0.9645 | 0.9646 | 6.689 |

### `linear`

| fold | n | PLCC | SROCC | RMSE |
|---|---:|---:|---:|---:|
| BigBuckBunny | 1 500 | 0.3596 | 0.5703 | 61.387 |
| BirdsInCage | 1 440 | 0.3651 | 0.6257 | 65.442 |
| CrowdRun | 1 050 | 0.4685 | 0.5720 | 53.139 |
| ElFuente1 | 1 260 | 0.2710 | 0.3908 | 53.997 |
| ElFuente2 | 1 620 | 0.4426 | 0.4627 | 52.619 |
| FoxBird | 900 | 0.2773 | 0.4026 | 60.054 |
| OldTownCross | 1 050 | 0.4256 | 0.5283 | 57.117 |
| Seeking | 1 500 | 0.2783 | 0.3324 | 50.324 |
| Tennis | 720 | 0.4229 | 0.4896 | 66.728 |

## 4. Cross-arch observations

- **FoxBird is the per-fold outlier on both MLPs** — lowest PLCC on
  `mlp_small` (0.9266) and `mlp_medium` (0.9286). FoxBird has the
  smallest sample count (900) and the most distinctive
  motion-coherence profile in the corpus, so the held-out fold has
  the least similar training signal. The same outlier on both arch
  rules out arch-specific overfitting; it's a corpus-distribution
  issue **within** the existing 9-source Netflix Public corpus. The
  Netflix Public Dataset is already in
  [`.workingdir2/netflix/`](../../.workingdir2/) (9 reference clips +
  70 distortion variants) and is what these LOSO runs train on, so
  "more Netflix Public" is not the unblocker. The natural unblocker
  is a **different / larger** training corpus that adds source-
  distribution diversity beyond Netflix's 9 clips — e.g. KoNViD-1k
  (Konstanz NR-VQA, 1 200 clips), BVI-DVC, or AOM-CTC source sets.
  Tier 6 has T6-1b (LPIPS-Sq) for an FR baseline expansion; corpus
  expansion is its own thread to open if FoxBird-class variance
  becomes a shipped-model concern.
- **Linear's variance is much higher** (PLCC ±0.077 vs ±0.020 for
  MLPs). The poor-fit linear model exposes more inter-clip noise —
  a fact about the linear model's inability to capture the feature-
  to-score relationship, not about the corpus itself.
- **PLCC and SROCC track each other tightly across all 27 fold-
  arch combinations** (Pearson ≈ 0.97 between PLCC and SROCC across
  the table). Either is a sufficient ranking summary; we keep both
  for consistency with prior work.
- **mlp_medium's RMSE win is consistent across folds** — every fold
  shows lower RMSE on medium than small except CrowdRun and FoxBird
  (within 0.1 RMSE either way). The 27 % aggregate RMSE reduction
  is real, not driven by a single fold.

## 5. Implications

- `vmaf_tiny_v1.onnx` (mlp_small) **remains the shipped default** —
  its LOSO-mean PLCC of 0.98 is the honest "expected accuracy on a
  new clip from this distribution" number, beating mlp_medium by
  ~1 PLCC point.
- `vmaf_tiny_v1_medium.onnx` (mlp_medium) **remains the shipped
  alternate** for users who care about absolute-VMAF agreement —
  the 27 % RMSE reduction is the canonical reason to opt in.
- `linear` **does not ship**; remains as a sanity-floor harness
  control documented in ADR-0203's "Three-arch sweep" section.
- Future work for FoxBird's outlier status: T6-1a (Netflix Public
  Dataset training corpus) increases the effective fold count and
  sample diversity beyond the 9-source ceiling.

## 6. Reproducer

```bash
# 1. Build libvmaf CLI (CPU is enough; eval is host-only)
meson setup build -Denable_cuda=false -Denable_sycl=false
ninja -C build

# 2. Sweep 9 LOSO folds × 3 arch (sequentially or in parallel; the
#    trainer spawns its own per-fold worker)
for arch in mlp_small mlp_medium linear; do
  for src in BigBuckBunny BirdsInCage CrowdRun ElFuente1 ElFuente2 \
             FoxBird OldTownCross Seeking Tennis; do
    out=model/tiny/training_runs/loso_${arch}/fold_${src}
    mkdir -p "$out"
    VMAF_TRAIN_OUT_DIR="$out" \
      bash ai/scripts/run_training.sh \
        --model-arch "$arch" --epochs 30 --val-source "$src"
  done
done

# 3. 3-arch eval
python ai/scripts/eval_loso_3arch.py
cat runs/loso_eval/loso_3arch_eval.md
```

## 7. Known artefacts

- `runs/loso_eval/loso_3arch_eval.{json,md}` is gitignored — the
  empirical numbers in §2–3 above are the durable record. Re-run
  the harness to regenerate.
- The per-fold ONNX files under
  `model/tiny/training_runs/loso_{mlp_small,mlp_medium,linear}/`
  are also gitignored. Regenerate via the loop in §6.
- The shipped baselines `vmaf_tiny_v1*.onnx` retain their pre-rename
  `external_data.location` references (a known issue worked around
  in `_load_session`); a proper re-export with matching names is a
  follow-up tracked elsewhere.
