<!-- markdownlint-disable MD013 -->
# Tiny AI — training data

This page documents the corpus path convention for the Netflix VMAF training
corpus, the `--data-root` loader API, and the recommended evaluation harness.
For the training *workflow* (feature extraction, model fit, export, eval),
see [training.md](training.md).

## Corpus location

Training data is **never committed**. All YUV files are gitignored. The
canonical local path for the Netflix corpus is:

```text
.workingdir2/netflix/
  ref/    # 9 reference YUVs
  dis/    # 70 distorted YUVs
```

The `--data-root` flag (or the `VMAF_DATA_ROOT` environment variable) tells
every training subcommand where to find the dataset. The flag takes
precedence when both are set.

KoNViD-1k uses a separate local root because the public dataset ships
MP4 clips rather than Netflix-style YUV reference/distorted pairs:

```text
$VMAF_KONVID_1K_DIR/
  KoNViD_1k_videos/    # 1200 source MP4s
```

`ai/scripts/konvid_to_full_features.py` also accepts `--konvid-root`;
when `VMAF_KONVID_1K_DIR` is unset it falls back to
`$VMAF_DATA_ROOT/konvid-1k` and then `~/datasets/konvid-1k`.

Dataset fetch helpers write run manifests beside the local data roots before
conversion starts. `ai/scripts/fetch_konvid_1k.py` writes
`<root>/fetch_manifest.json` by default and accepts `--manifest-out`.
`ai/scripts/fetch_youtube_ugc_subset.py` keeps its existing stem-to-files
content manifest and writes a separate `<manifest>.run-manifest.json` sidecar
unless `--run-manifest-out` is supplied. Keep these gitignored sidecars with
the downloaded corpora when citing training evidence.

### Naming convention

Files follow the Netflix encoding-ladder convention:

```text
<source>_<quality_label>_<height>_<bitrate-kbps>.yuv
```

For example:

```text
ref/
  BigBuck_0_576_0.yuv        # source, quality 0 = pristine, 576 lines
dis/
  BigBuck_1_576_2000.yuv     # source, quality 1, 576 lines, 2000 kbps
  BigBuck_2_576_1000.yuv     # source, quality 2, 576 lines, 1000 kbps
```

The `<quality_label>` is an opaque integer assigned at encode time; 0
conventionally means the lossless or near-lossless reference.

## Loader API

The loader is implemented in
[`ai/src/vmaf_train/data/datasets.py`](../../ai/src/vmaf_train/data/datasets.py).
When `--data-root` points to a directory with the layout above, the loader:

1. Scans `<data-root>/ref/` and `<data-root>/dis/` for `.yuv` files.
2. Pairs each distorted file with its reference by matching the `<source>`
   component (first underscore-separated token).
3. Invokes `libvmaf` via subprocess to extract the six-element feature vector
   per frame: `[ vif_scale0, vif_scale1, vif_scale2, vif_scale3, motion2, adm2 ]`.
4. Mean-pools the per-frame vectors to produce one clip-level vector.
5. Caches the result as a Parquet file under
   `<data-root>/.cache/nflx_features.parquet` so repeated invocations skip
   the expensive libvmaf pass.

### Invoking the loader

```bash
# Extract features from the local Netflix corpus.
vmaf-train extract-features \
    --data-root .workingdir2/netflix \
    --dataset nflx-local \
    --vmaf-binary core/build-cpu/tools/vmaf \
    --output ai/data/nflx_local_features.parquet

# If VMAF_DATA_ROOT is set instead:
export VMAF_DATA_ROOT=.workingdir2/netflix
vmaf-train extract-features --dataset nflx-local \
    --output ai/data/nflx_local_features.parquet
```

The `--dataset nflx-local` token tells the loader to use the
`NflxLocalDataset` class, which reads the directory layout above instead of
downloading from a URL.

## Evaluation harness

After extraction, fit and evaluate a model against the teacher soft-label
baseline (resolved from the ADR-1168 single source `DEFAULT_MODEL`, or specified
via `--assume-teacher` for legacy datasets; ADR-1173):

```bash
# 1. Train.
vmaf-train fit \
    --config ai/configs/fr_tiny_v1.yaml \
    --features ai/data/nflx_local_features.parquet \
    --output runs/fr_tiny_v2_nflx/

# 2. Export.
vmaf-train export \
    --checkpoint runs/fr_tiny_v2_nflx/last.ckpt \
    --output model/tiny/vmaf_tiny_fr_v2_nflx.onnx \
    --opset 17

# 3. Evaluate on the held-out 10-clip test split.
vmaf-train eval \
    --model model/tiny/vmaf_tiny_fr_v2_nflx.onnx \
    --features ai/data/nflx_local_features.parquet \
    --split test
# Reports PLCC, SROCC, RMSE vs teacher soft labels.

# 4. One-command MCP server health check (ADR-0242).
cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v
```

## Data path safety invariants

- **Never commit YUV files.** The `.gitignore` at the repo root lists
  `*.yuv` and `.workingdir2/`. Do not override these entries.
- The training script takes `--data-root` as an explicit CLI flag precisely
  to avoid hard-coding the local path. CI does not have the corpus; the
  smoke test in `test_smoke_e2e.py` uses only the committed Netflix golden
  fixture (`python/test/resource/yuv/src01_hrc00_576x324.yuv`), not the
  training corpus.
- The `NflxLocalDataset` class validates that each YUV is under
  `<data-root>/` before reading it, preventing path-traversal issues
  (SEI CERT FIO02-C).

## Split reproducibility

The train/test split is keyed by a deterministic hash of the clip's
relative path within `<data-root>/dis/`. This means the same clip always
lands in the same split regardless of directory enumeration order,
satisfying the reproducibility invariant documented in `docs/ai/training.md §
Determinism`.

## See also

- [training.md](training.md) — full training workflow
- [inference.md](inference.md) — running the trained ONNX model via C API or CLI
- [ADR-0242](../adr/0242-tiny-ai-netflix-training-corpus.md) — architecture
  and distillation policy decisions
- [ADR-0417](../adr/0417-tiny-ai-netflix-training-scaffold-pr.md) — draft PR
  registration; consult before triggering a training run
- [Research digest 0019](../research/0019-tiny-ai-netflix-training.md) —
  VMAF methodology survey and distillation literature (2026-04-27)
- [Research digest 0099](../research/0099-tiny-ai-netflix-training-update.md) —
  2024–2026 distillation, ONNX Runtime, and lightweight FR regressor update
- [Research digest 0607](../research/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md) —
  2024–2026 refresh: EfficientVMAF, temperature-scaled distillation, ORT 1.19/1.20,
  feature-reweighting (2026-05-19)
- [ADR-0612](../adr/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md) — architecture
  alternatives table and training-data contract formalised (2026-05-19 scaffold iteration)
- [ADR-0640](../adr/0640-tiny-ai-netflix-training-scaffold-2026-05-20.md) — EfficientVMAF
  survey update, feature-reweighting alternative added (2026-05-20 scaffold iteration)
- [Research digest 0615](../research/0615-tiny-ai-netflix-training-2026-05-20.md) —
  EfficientVMAF (CVPR 2024), IQA-PyTorch distillation, ORT 1.20 update (2026-05-20)
