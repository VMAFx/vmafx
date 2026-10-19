<!-- markdownlint-disable MD013 MD060 -->
# `vmaf-tune predict`, predictor training and the local sidecar

`vmaf-tune predict` estimates the VMAF of each shot of a video without
encoding every shot to the end. It probe-encodes each shot, runs a
learned ONNX predictor (or an analytical fallback), checks the result
against real libvmaf scores on a few shots, and prints a verdict. The
same predictors feed the [`fast`](vmaf-tune-fast-path.md),
[per-shot](vmaf-tune-per-shot.md), [ladder](vmaf-tune-ladder.md) and
[auto](vmaf-tune-auto.md) paths. Overview: [vmaf-tune.md](vmaf-tune.md).

## Predict and validate

Run the predictor on a container source and check it on 8 shots:

```shell
vmaf-tune predict \
    --source source.mp4 \
    --codec libx264 \
    --target-vmaf 96 \
    --report-out predict-report.json
```

The command needs `ffmpeg`, `ffprobe` and the `vmaf-perShot` binary
(`--per-shot-bin`) for shot detection. When `vmaf-perShot` is
unavailable, pass `--total-frames` for the single-shot fallback.

The report is JSON, written to `--report-out` or to stdout:

| Key | Meaning |
|-----|---------|
| `verdict` | `gospel`, `recalibrate` or `fall_back`. |
| `target_vmaf` | Target the run was validated against. |
| `residual_threshold` | Maximum acceptable absolute residual. |
| `max_abs_residual` / `mean_residual` | Residual statistics over the validated shots. |
| `bias_correction` | Signed VMAF offset to add to predictions; set only when the verdict is `recalibrate`, otherwise `0.0`. |
| `k_validated` | Number of shots checked against real libvmaf. |
| `uncertainty` | `enabled`, `calibrated` and `alpha` of the conformal interval. |
| `residuals` | One row per validated shot; each gains an `interval` of `{low, high, alpha}` with `--with-uncertainty`. |

Exit codes: `0` for `gospel` or `recalibrate`, `2` for `fall_back`, `1`
when the source cannot be probed or no shots are detected. A predictor
model that is a synthetic stub prints a warning on stderr; it is not
authoritative for production CRF picks.

### Flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--source PATH` | required | Reference video, any FFmpeg-readable container. |
| `--codec NAME` | `libx264` | Codec adapter; one of the 19 registered adapters. |
| `--target-vmaf F` | `93.0` | Target pooled-mean VMAF. |
| `--validate-k N` | `8` | Shots verified against real libvmaf. |
| `--residual-threshold F` | `1.5` | Largest `abs(predicted - measured)` VMAF before falling back. |
| `--model PATH` | none | `predictor_<codec>.onnx`; without it the analytical fallback runs. |
| `--use-saliency` | off | Add saliency mean and variance to the predictor features. |
| `--saliency-model PATH` | `model/tiny/saliency_student_v1.onnx` | Saliency ONNX for `--use-saliency`. |
| `--bitdepth {8,10,12}` | `8` | Source bit depth, forwarded to `vmaf-perShot`. |
| `--total-frames N` | `0` | Frame count for the single-shot fallback. |
| `--per-shot-bin PATH` | `vmaf-perShot` | `vmaf-perShot` binary. |
| `--ffmpeg-bin PATH` | `ffmpeg` | ffmpeg binary. |
| `--ffprobe-bin PATH` | `ffprobe` | ffprobe binary. |
| `--report-out PATH` | stdout | Destination of the validation report. |
| `--with-uncertainty` | off | Emit conformal prediction intervals next to each point estimate. |
| `--calibration-sidecar PATH` | none | Split-conformal calibration JSON from `vmaftune.conformal.save_split_calibration`; read only with `--with-uncertainty`. |
| `--alpha F` | from the sidecar | Overrides the nominal miscoverage level (`0.05` is 95 % coverage); ignored without `--with-uncertainty`. |

Without a calibration sidecar, `--with-uncertainty` degrades to
`low == high == point` and the report is flagged uncalibrated. Background
on the intervals: [conformal VQA](../ai/conformal-vqa.md); predictor API:
[predictor](../api/predictor.md).

### Saliency features

`--use-saliency` decodes each sampled shot to temporary `yuv420p`, runs
the saliency ONNX and feeds only `saliency_mean` and `saliency_var` into
the predictor:

```shell
vmaf-tune predict \
    --source source.mp4 \
    --codec libx264 \
    --target-vmaf 96 \
    --use-saliency \
    --saliency-model model/tiny/saliency_student_v1.onnx
```

!!! note
    This flag is separate from `recommend-saliency --saliency-aware`,
    which creates ROI and QP sidecars for the encoder. See
    [saliency-aware](vmaf-tune-saliency-aware.md).

## Train predictors from a corpus

`vmaf_train.predictor_train` trains the per-codec ONNX predictors. It belongs
to the ai/ training package (`pip install -e ai`), the only place besides the
ensemble kit where torch is installed
([ADR-1886](../adr/1886-torch-training-environments-only.md)); vmaf-tune itself
only loads the trained ONNX models.
`--corpus` accepts one JSONL file or a directory of JSONL shards;
directories are scanned recursively in sorted order, so the trainer can
read `.corpus/corpus_run/` directly:

```shell
python -m vmaf_train.predictor_train \
    --corpus .corpus/corpus_run \
    --codec libx264 \
    --output-dir .workingdir/evidence/predictor-real
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--corpus PATH` | none | JSONL file or directory of shards. |
| `--output-dir PATH` | `model` | Where `predictor_<codec>.onnx` and the model cards go. |
| `--codec NAME` | all 14 trainable codecs | Restrict training; repeatable. |
| `--epochs`, `--batch-size`, `--lr`, `--seed`, `--val-fraction`, `--opset` | `TrainConfig` defaults | Training hyper-parameters and ONNX opset. |
| `--emit-stub-card-only DEST` | none | Write a synthetic-stub model card for the first `--codec` to `DEST` (`-` for stdout) and exit without training. |

Corpus handling:

- Rows are filtered per codec after schema aliases are normalised. The
  trainer accepts current [corpus](vmaf-tune-corpus.md) rows (`encoder`,
  `crf`, `vmaf_score`, `bitrate_kbps`) and older hardware-sweep rows
  (`codec`, `q` or `cq`, `vmaf`, `actual_kbps`).
- A codec with no usable rows falls back to the documented synthetic-stub
  corpus, and its model card records `corpus.kind: synthetic-stub-*`. A
  codec with rows in any shard records `corpus.kind: real-N=<rows>`.
- Richer predictor inputs are preserved instead of zero-filled:
  `probe_i_frame_avg_bytes`, `probe_p_frame_avg_bytes`,
  `probe_b_frame_avg_bytes`, `saliency_mean`, `saliency_var`,
  `frame_diff_mean`, `y_avg` and `y_var`. Older rows stay valid: missing
  probe-byte columns use deterministic bitrate stand-ins, and missing
  saliency or signalstats columns stay `0.0`.

## Local sidecar bias correction

`vmaf-tune sidecar` trains a small correction on your own host from the
residuals between predicted and observed VMAF. It never uploads captures
and never changes the shipped predictor. State is stored under
`${XDG_CACHE_HOME:-~/.cache}/vmaf-tune/sidecar/`. Background and
algorithm: [local sidecar training](../ai/local-sidecar-training.md).

The four actions share these flags:

| Flag | Default | Meaning |
|------|---------|---------|
| `--codec NAME` | `libx264` | Codec bucket of the sidecar state. |
| `--cache-dir PATH` | the XDG cache path above | Sidecar state directory. |
| `--predictor-version V` | `predictor_v1` | Predictor the sidecar corrects; a version mismatch resets the sidecar to cold start. |
| `--model PATH` | none | Predictor ONNX to correct. |
| `--json` | off | Print the result as JSON. |

1. Inspect the current state:

    ```shell
    vmaf-tune sidecar status --codec libx264 --json
    ```

2. Record one observed encode. `features.json` is either a flat
   `ShotFeatures` object or `{ "features": { ... } }`. The required
   fields are `probe_bitrate_kbps`, `probe_i_frame_avg_bytes`,
   `probe_p_frame_avg_bytes` and `probe_b_frame_avg_bytes`. Add
   `--no-persist` to update in memory without saving:

    ```shell
    vmaf-tune sidecar record \
        --codec libx264 \
        --features-json features.json \
        --crf 28 \
        --observed-vmaf 94.2
    ```

3. Or train in batch from a JSONL file with one observed encode per
   line:

    ```json
    {"features":{"probe_bitrate_kbps":3000,"probe_i_frame_avg_bytes":10000,"probe_p_frame_avg_bytes":2000,"probe_b_frame_avg_bytes":1000,"width":1920,"height":1080,"fps":24},"crf":28,"observed_vmaf":94.2}
    ```

    ```shell
    vmaf-tune sidecar batch-record --codec libx264 --captures-jsonl captures.jsonl
    ```

4. Predict with the correction. The output lists the bare predictor
   score (`base_vmaf`), the sidecar correction and the final clamped
   score (`sidecar_vmaf`):

    ```shell
    vmaf-tune sidecar predict --codec libx264 --features-json features.json --crf 28 --json
    ```

## See also

- [Overview](vmaf-tune.md)
- [Corpus](vmaf-tune-corpus.md)
- [Fast path](vmaf-tune-fast-path.md)
