<!-- markdownlint-disable MD013 -->
# Tiny AI — training data

This page documents the local layout of the Netflix VMAF training corpus, the
loader that reads it, and how to evaluate a model trained on it. For the
training workflow itself (feature extraction, fit, export, eval) see
[training.md](training.md).

## Corpus location

Training data is never committed. All YUV files are gitignored. The canonical
local path for the Netflix corpus is:

```text
.corpus/netflix/
  ref/    # 9 reference YUVs
  dis/    # 70 distorted YUVs
```

KoNViD-1k uses a separate local root, because the public dataset ships MP4 clips
rather than Netflix-style YUV reference and distorted pairs:

```text
$VMAF_KONVID_1K_DIR/
  KoNViD_1k_videos/    # 1200 source MP4s
```

`ai/scripts/konvid_to_full_features.py` also accepts `--konvid-root`. When
`VMAF_KONVID_1K_DIR` is unset it falls back to `$VMAF_DATA_ROOT/konvid-1k` and
then `~/datasets/konvid-1k`.

Dataset fetch helpers write run manifests beside the local data roots before
conversion starts:

- `ai/scripts/fetch_konvid_1k.py` writes `<root>/fetch_manifest.json` by default
  and accepts `--manifest-out`.
- `ai/scripts/fetch_youtube_ugc_subset.py` keeps its existing stem-to-files
  content manifest and writes a separate `<manifest>.run-manifest.json` sidecar
  unless `--run-manifest-out` is supplied.

Keep these gitignored sidecars with the downloaded corpora when citing training
evidence.

### Naming convention

The loader (`ai/data/netflix_loader.py`) expects these file names:

```text
ref/<source>_<fps>fps.yuv
dis/<source>_<quality>_<height>_<bitrate-kbps>.yuv
```

For example:

```text
ref/
  BigBuckBunny_25fps.yuv        # one reference per source
dis/
  BigBuckBunny_1_576_2000.yuv   # source, quality 1, 576-line ladder rung, 2000 kbps
```

`<quality>` is an opaque integer assigned at encode time. All corpus YUVs are
1920x1080 `yuv420p` 8-bit regardless of the `<height>` token, which records the
encode-ladder rung before upscale to the reference resolution. The loader checks
the file size against that assumption and falls back to ffprobe when it does not
match.

## Loader

Two loaders read the corpus:

| Entry point | Used by | Reads |
| --- | --- | --- |
| `ai/train/dataset.py::NetflixFrameDataset` | `ai/train/train.py`, `ai/train/train_combined.py`, `--data-root` | `<data-root>/ref/` and `<data-root>/dis/` YUVs |
| `vmaf-train` datasets and manifests (`ai/src/vmaf_train/data/datasets.py`) | `vmaf-train extract-features --dataset nflx`, `manifest-scan` | A SHA-256 manifest of a dataset under `$VMAF_DATA_ROOT` (default `~/.cache/vmaf-train`) |

`NetflixFrameDataset` works as follows:

1. Scan `<data-root>/ref/` and `<data-root>/dis/` for `.yuv` files and pair each
   distorted file with its reference by the `<source>` token. A distorted file
   without a reference is skipped.
2. Run libvmaf through a subprocess to extract the six-element feature vector
   per frame, and the teacher score per frame.
3. Cache the per-clip features and scores as JSON under
   `~/.cache/vmaf-tiny-ai/<source>/<dis_basename>.json`. Override the cache root
   with `VMAF_TINY_AI_CACHE`. A cache entry whose stamped teacher differs from
   the resolved teacher is recomputed, see
   [training.md](training.md#teacher-model-provenance).

Each sample is one frame: a feature vector paired with the teacher per-frame
score in `[0, 100]`.

`--data-root` exists on `ai/train/train.py` and `train_combined.py`
(`--netflix-root`). The `vmaf-train` subcommands take no `--data-root`.
`VMAF_DATA_ROOT` is the dataset cache root of the `vmaf-train` manifests, not a
substitute for `--data-root`.

### Use the `vmaf-train` manifest path

1. Scan the corpus into the `nflx` manifest:

    ```bash
    vmaf-train manifest-scan --dataset nflx --root $VMAF_DATA_ROOT/nflx
    ```

2. Extract features:

    ```bash
    vmaf-train extract-features \
        --dataset nflx \
        --vmaf-binary core/build-cpu/tools/vmaf \
        --output ai/data/nflx_features.parquet
    ```

## Evaluation harness

After extraction, fit and evaluate a model against the teacher soft labels. The
teacher is resolved from the ADR-1168 single source `DEFAULT_MODEL`, or set with
`--assume-teacher` for legacy datasets (ADR-1173).

1. Train:

    ```bash
    vmaf-train fit \
        --config ai/configs/fr_tiny_v1.yaml \
        --cache ai/data/nflx_features.parquet \
        --output runs/fr_tiny_nflx/
    ```

2. Export:

    ```bash
    vmaf-train export \
        --checkpoint runs/fr_tiny_nflx/last.ckpt \
        --output runs/fr_tiny_nflx/fr_tiny_nflx.onnx \
        --opset 17
    ```

3. Evaluate on the held-out test split. The command reports PLCC, SROCC and RMSE
   against the labels in the feature table:

    ```bash
    vmaf-train eval \
        --model runs/fr_tiny_nflx/fr_tiny_nflx.onnx \
        --features ai/data/nflx_features.parquet \
        --split test
    ```

4. Run the MCP server health check (ADR-0242):

    ```bash
    cd mcp-server/vmaf-mcp && python -m pytest tests/test_smoke_e2e.py -v
    ```

## Data path safety invariants

- **Never commit YUV files.** The `.gitignore` at the repo root lists `*.yuv`
  and
  `.corpus/`. Do not override these entries.
- The training script takes `--data-root` as an explicit CLI flag to avoid
  hard-coding the local path. CI does not have the corpus; the smoke test in
  `test_smoke_e2e.py` uses only the committed Netflix golden fixture
  (`python/test/resource/yuv/src01_hrc00_576x324.yuv`), not the training corpus.

## Split reproducibility

`ai/train/` holds out one source for validation: `--val-source`, default
`Tennis`. The split is deterministic, because the hold-out source name is the
only knob (ADR-0203). It is content-disjoint: with 9 sources the hold-out gives
about 88 % train and 12 % validation by clip count. A random frame split would
leak frames of one clip into both halves and inflate PLCC by 5 to 10 percentage
points. For leave-one-source-out over all 9 sources see
[loso-eval.md](loso-eval.md). `vmaf-train eval` splits by a deterministic hash of
the clip key (fixed salt `vmaf-train-splits-v1`), so the same clip always lands
in the same split, as described in
[training.md](training.md#determinism).

## Dataset terms

Some shipped tiny models were trained on datasets that publish terms of their
own. This section quotes each dataset's terms verbatim, as the dataset states
them (pages read on 2026-10-04), and names the models trained on it. Each
model card repeats the quote next to the fork's reading of it
([ADR-1570](../adr/1570-tiny-model-dataset-terms-retrain-rc9.md)).

The fork keeps these models under the licences recorded in
[`model/tiny/registry.json`](https://github.com/vidiko/vmafx/blob/master/model/tiny/registry.json).
Its reading is the same for every dataset: the weights are fitted
parameters, they cannot reproduce a clip, an image or a label, and no dataset
file is redistributed. That reading is the fork's; it is not a
permission from the dataset's authors. Where a dataset restricts its use to
research, the restriction is quoted, and if it binds the trained weights where
you use them, treat the model as research-only or retrain it on data you are
cleared to use. The fork retrains these models on cleared data in RC9
(`T-TINY-AI-RETRAIN-CLEARED-DATA-2026-10-04` in [state](../state.md),
[ADR-1490](../adr/1490-rc3-rc9-candidate-map-cpu-capability.md)).

### Netflix Public Dataset

Source: `resource/doc/datasets.md` of
[Netflix/vmaf](https://github.com/Netflix/vmaf/blob/0fb4152418d0351901e9c5fd2d30668dced89cdb/resource/doc/datasets.md)
(commit `0fb41524`, the page's last change); the fork's copy is
[datasets](../models/datasets.md#netflix-public-dataset).

> We provide a dataset publicly available to the community for training, testing and verification of results purposes.
>
> (please request for access and we will grant it)

The page states no other terms. Trained on it: `fr_regressor_v1`,
`fr_regressor_v2` and `fr_regressor_v3` (the vmaf-tune Phase A encodes of its
nine references), `vmaf_tiny_v1`, `vmaf_tiny_v1_medium`, `vmaf_tiny_v2`,
`vmaf_tiny_v3` and `vmaf_tiny_v4`.

### KoNViD-1k

Source: the [KoNViD-1k database page](http://database.mmsp-kn.de/konvid-1k-database.html).

> KoNViD-1k is freely available to the research community.
>
> We took YFCC100m as a baseline database, consisting of 793436 Creative Commons (CC) video sequences

The page names no licence and offers the database to the research community.
Each clip keeps the Creative Commons licence of its YFCC100M upload, and those
licences differ between clips. Trained on it: `nr_metric_v1`,
`learned_filter_v1`, `vmaf_tiny_v2`, `vmaf_tiny_v3` and `vmaf_tiny_v4`.

### BVI-DVC

Source: the copyright notice
[`BVI-DVC.txt`](https://fan-aaron-zhang.github.io/assets/copyrights/BVI-DVC.txt),
which the [BVI-DVC page](https://fan-aaron-zhang.github.io/BVI-DVC/) asks you
to read "before using the database and for copyright permissions".

> The 800 videos can be used to train CNN models for deep video compression or other computer video tasks, such as image/video super-resolution, image/video enhancement, video frame interpolation, etc..
>
> This database has been compiled by the University of Bristol, Bristol, UK, comprising sequences originally generated by various sources. All intellectual property rights remain with the originators of each sequence. The test sequences from source (15) mentioned above shall only be used for academic research (no commercial use). Material from other sources can also be employed for developing future video coding standards and for evaluating performance of test models in JVET and the subsequent standardization project, and for the relational parent body activities. This copyright and permission notice shall be duplicated whenever the data is copied. The University of Bristol makes no warranties with respect to the material and expressly disclaims any warranties regarding its fitness for any purpose. Unless the above conditions are agreed to by the recipient, no permission is granted for any use and copying of the data. By using the database and sequences, the user agrees to the conditions of this copyright and disclaimer.

Source (15) is the Ultra Video Group (Tampere University) database: its
sequences are for academic research only. Trained on subsets A to D:
`vmaf_tiny_v2`, `vmaf_tiny_v3` and `vmaf_tiny_v4`.

### DUTS-TR (images from ImageNet)

Source: the [DUTS page](http://saliencydetection.net/duts/).

> All rights reserved by the original authors of DUTS Image Dataset.
>
> All training images are collected from the ImageNet DET training/val sets

The DUTS-TR images are ImageNet images, so ImageNet's terms (next section)
apply to them as well. Trained on it: `saliency_student_v1` and
`saliency_student_v2`.

### ImageNet

Source: ImageNet's [terms of access](https://image-net.org/download.php), which
limit the images to non-commercial research and education.

> Researcher shall use the Database only for non-commercial research and educational purposes.

Its images reach three models: `saliency_student_v1` and `saliency_student_v2`
through DUTS-TR, and `lpips_sq_v1` through torchvision's ImageNet-trained
weights.

### ImageNet through torchvision's pretrained weights

`lpips_sq_v1` carries torchvision's ImageNet-trained SqueezeNet 1.1 features.
The [torchvision documentation](https://docs.pytorch.org/vision/stable/models.html)
(0.29) says of its pretrained weights:

> The pre-trained models provided in this library may have their own licenses or terms and conditions derived from the dataset used for training. It is your responsibility to determine whether you have permission to use the models for your use case.

ImageNet's terms of access, quoted above, are those terms.

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
- [Research digest
  0612](../research/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md) —
  2024–2026 refresh: EfficientVMAF, temperature-scaled distillation, ORT
  1.19/1.20,
  feature-reweighting (2026-05-19)
- [ADR-0612](../adr/0612-tiny-ai-netflix-training-scaffold-2026-05-19.md) —
  architecture
  alternatives table and training-data contract formalised (2026-05-19 scaffold
  iteration)
- [ADR-0640](../adr/0640-tiny-ai-netflix-training-scaffold-2026-05-20.md) —
  EfficientVMAF
  survey update, feature-reweighting alternative added (2026-05-20 scaffold
  iteration)
- [Research digest
  0615](../research/0615-tiny-ai-netflix-training-2026-05-20.md) —
  EfficientVMAF (CVPR 2024), IQA-PyTorch distillation, ORT 1.20 update
  (2026-05-20)
