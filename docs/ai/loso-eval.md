# Tiny-AI — LOSO evaluation

Leave-one-source-out (LOSO) evaluation of the `mlp_small` regressor
on the Netflix corpus, using the harness in
[`ai/scripts/eval_loso_mlp_small.py`](../../ai/scripts/eval_loso_mlp_small.py).

LOSO is the honest evaluation regime for a 9-source corpus: each fold
trains on 8 sources and is scored on the held-out 9th. The single-split
val=Tennis baseline `vmaf_tiny_v1.onnx` (and its `mlp_medium` sibling)
remain shipped as the default tiny model, but their absolute-fit numbers
are biased by their training distribution. LOSO answers *"how well does
mlp_small actually generalize to a clip it has never seen?"* — which is
the user-facing question.

## What the harness does

1. Loads each of the 9 source clips' `(features, vmaf_v0.6.1)` arrays via
   `ai.train.dataset.NetflixFrameDataset(split="val", val_source=clip)`.
   Cached per-clip JSON under `~/.cache/vmaf-tiny-ai/<source>/` is
   reused; cold runs invoke the libvmaf CLI to populate the cache.
2. For each fold, loads
   `model/tiny/training_runs/loso_mlp_small/fold_<clip>/mlp_small_final.onnx`
   and scores it on its own held-out clip. Reports per-fold PLCC /
   SROCC / RMSE plus mean ± std across the 9 folds.
3. For each baseline (`vmaf_tiny_v1.onnx`,
   `vmaf_tiny_v1_medium.onnx`), scores per-clip and on the all-clips
   concatenation. The all-clips concat shows the score-axis offset
   that is hidden by per-clip evaluation.

The harness mirrors the per-fold accounting that MCP
[`compare_models`](../mcp/index.md) does for a single split, but
respects the LOSO split structure (each model has a different val
clip) without forcing 9 separate `compare_models` calls.

## Running it

```bash
# Default — uses .workingdir2/netflix as data-root, looks for fold ONNX
# under model/tiny/training_runs/loso_mlp_small, writes to runs/loso_eval/
python ai/scripts/eval_loso_mlp_small.py

# Explicit data root + custom output dir
python ai/scripts/eval_loso_mlp_small.py \
    --data-root /path/to/netflix-corpus \
    --out /tmp/loso-eval

# Use a different fold-output directory (e.g. mlp_medium LOSO sweep)
python ai/scripts/eval_loso_mlp_small.py \
    --loso-dir model/tiny/training_runs/loso_mlp_medium
```

Outputs:

- `runs/loso_eval/loso_mlp_small_eval.json` — machine-readable, including a
  `run_provenance` block with the script entrypoint, argv, parsed arguments,
  data root, fold directory, baseline ONNX inputs, and JSON/Markdown output
  targets.
- `runs/loso_eval/loso_mlp_small_eval.md` — markdown summary.

The 3-architecture companion harness
[`ai/scripts/eval_loso_3arch.py`](../../ai/scripts/eval_loso_3arch.py) writes
`runs/loso_eval/loso_3arch_eval.json` and
`runs/loso_eval/loso_3arch_eval.md`; its JSON report carries the same
`run_provenance` schema and records the shared `--training-runs-dir` input.

Both directories (`runs/` and `model/tiny/training_runs/`) are
gitignored by design — training and eval outputs regenerate from
the corpus and the cached features, so they don't belong in the
tree.

## Producing the fold checkpoints

The 9 fold ONNX files are produced by looping the existing trainer
across each `--val-source`:

```bash
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
```

On a populated `~/.cache/vmaf-tiny-ai/`, each fold takes ~6 min on
the documented `ryzen-4090` profile; total ~55 min. With a cold cache
add ~30 s of libvmaf feature extraction per source-clip on first
encounter.

## Reading the results

Per-fold PLCC of 0.93 – 0.99 indicates the regressor learns a
generalizable feature → score mapping; the mean across folds is the
honest "expected accuracy on a new clip" number. Per-fold RMSE is on
the VMAF-score axis (`y ∈ [0, 100]`); a per-fold RMSE of ~15 is
expected when the regressor has not seen the clip's score-distribution
shape.

The single-split baselines (`vmaf_tiny_v1.onnx`,
`vmaf_tiny_v1_medium.onnx`) score higher per-clip on the clips they
trained on, but their all-clips-concat PLCC drops because the score
axis is offset differently across clips and the baselines never
learned to align them. This is the reason LOSO is the more honest
metric.

For a full discussion of the methodology, the corpus, and the
numbers from this run, see
[Research Digest 0022](../research/0022-loso-mlp-small-results.md).

## Known issues

The shipped baseline ONNX files
(`model/tiny/vmaf_tiny_v1*.onnx`) carry an embedded
`external_data.location` reference to their pre-rename filename
(`mlp_small_final.onnx.data` / `mlp_medium_final.onnx.data`). The
sibling `.data` file actually exists under the renamed name, so a
naive `onnxruntime.InferenceSession(path)` from outside `model/tiny/`
fails with `cannot get file size`. The harness works around this in
[`_load_session`](../../ai/scripts/eval_loso_mlp_small.py): it loads
the proto without external data, rewrites the location entries to
the actual sibling filename, and attaches the bytes manually. A
proper fix is a follow-up baseline re-export with the correct
embedded names.
