# `ai/scripts/` corpus-path environment variables

Set an environment variable to point an `ai/scripts/` script at your own corpus
directory, with no CLI edit. Every corpus-ingestion and training script under
`ai/scripts/` defaults to a `.corpus/<corpus>/` directory inside the repository.
That directory is gitignored and absent on a fresh checkout, so on a host
without the corpora (including the
[`vmaf-dev-mcp` container](../development/dev-mcp.md)) the first run fails with
`FileNotFoundError`.

Per [ADR-0547](../adr/0547-ai-script-env-vars.md) each script accepts an env-var
override layered on top of the default. Leave the variable unset and the
`.corpus/` default applies.

## Overrides

| Script(s) | Env var | Default |
| --- | --- | --- |
| `chug_to_corpus_jsonl.py`, `chug_extract_features.py`, `train_chug_hdr_mos_head.py` (input shards) | `VMAF_CHUG_DIR` | `<repo>/.corpus/chug` |
| `train_chug_hdr_mos_head.py` (local model outputs) | `VMAF_CHUG_OUTPUT_DIR` | `<repo>/.corpus/chug` |
| `konvid_1k_to_corpus_jsonl.py`, `konvid_to_full_features.py`, `train_konvid_mos_head.py` (1k input) | `VMAF_KONVID_1K_DIR` | `<repo>/.corpus/konvid-1k`; full-feature extraction falls back to `$VMAF_DATA_ROOT/konvid-1k` when unset |
| `konvid_150k_to_corpus_jsonl.py`, `extract_k150k_features.py`, `train_konvid_mos_head.py` (150k input), `train_predictor_v2_realcorpus.py` | `VMAF_KONVID_150K_DIR` | `<repo>/.corpus/konvid-150k` |
| `lsvq_to_corpus_jsonl.py` | `VMAF_LSVQ_DIR` | `<repo>/.corpus/lsvq` |
| `live_vqc_to_corpus_jsonl.py` | `VMAF_LIVE_VQC_DIR` | `<repo>/.corpus/live-vqc` |
| `youtube_ugc_to_corpus_jsonl.py` | `VMAF_YOUTUBE_UGC_DIR` | `<repo>/.corpus/youtube-ugc` |
| `waterloo_ivc_to_corpus_jsonl.py` | `VMAF_WATERLOO_IVC_DIR` | `<repo>/.corpus/waterloo-ivc-4k` |
| `extract_full_features.py`, `eval_loso_mlp_small.py`, `eval_loso_3arch.py`, `validate_ensemble_seeds.py`, `train_predictor_v2_realcorpus.py` | `VMAF_NETFLIX_CORPUS_DIR` | `<repo>/.corpus/netflix` |
| `train_predictor_v2_realcorpus.py` | `VMAF_BVI_DVC_RAW_DIR` | `<repo>/.corpus/bvi-dvc-raw` |
| `bvi_dvc_to_full_features.py` | `VMAF_BVI_DVC_ZIP` | `<repo>/.corpus/bvi-dvc-raw/BVI-DVC Part 1.zip` |

## Other `ai/` environment variables

| Env var | Default | Read by |
| --- | --- | --- |
| `VMAF_DATA_ROOT` | `~/.cache/vmaf-train` | `vmaf-train` dataset cache and manifests ([training.md](training.md#dataset-acquisition)); `fetch_konvid_1k.py` uses `$VMAF_DATA_ROOT/konvid-1k` (else `~/datasets/konvid-1k`) |
| `VMAF_TINY_AI_CACHE` | `~/.cache/vmaf-tiny-ai` | `ai/train/` per-clip feature cache ([training.md](training.md#netflix-corpus-flow)) |
| `VMAF_TINY_AI_CACHE_KONVID_FULL` | `~/.cache/vmaf-tiny-ai-konvid-full` | `konvid_to_full_features.py` `--cache-dir` |
| `VMAF_TINY_AI_CACHE_BVI_DVC_FULL` | `$XDG_CACHE_HOME/vmaf-tiny-ai-bvi-dvc-full` | `bvi_dvc_to_full_features.py` `--cache-dir` |
| `VMAF_TINY_AI_SCRATCH` | system temp directory | `extract_ugc_features.py`, `export_transnet_v2.py` scratch files; an empty value is an error |
| `VMAF_MODEL_PATH` | unset | teacher model JSON override, step 2 of the resolution order in [training.md](training.md#teacher-model-provenance) |
| `VMAF_BIN` | `core/build-cpu/tools/vmaf` | `run_training.sh` vmaf binary |
| `VMAF_CORPUS_DIR` | `.corpus/netflix` | `calibrate_nr_threshold.py` corpus directory |
| `VMAF_CHUG_HDR_ONNX` | git-ignored `.workingdir` evidence path | `validate_chug_hdr_mos_head.py` ([chug-hdr-held-out-validator.md](chug-hdr-held-out-validator.md)) |
| `VMAFX_RUNS_DIR` | `<repo>/runs` | `train_fr_regressor_v2.py` default metrics path |
| `VMAFX_PIPELINE_HASH` | derived from git | `aiutils.parquet_utils` pipeline-hash metadata override |
| `VMAF_HW_TAG` | `ryzen-9950x3d+rtx4090+arc-a380` | `measure_quant_drop_per_ep.py` hardware tag ([quant-eps.md](quant-eps.md)) |
| `VMAFX_SIDECAR_*` | see the table | online trainer, [sidecar-online-training.md](sidecar-online-training.md#configuration-reference) |

## Usage examples

```bash
# Inside the dev-mcp container with corpora bind-mounted under /workspace
export VMAF_CHUG_DIR=/workspace/chug
export VMAF_KONVID_150K_DIR=/workspace/konvid-150k
export VMAF_NETFLIX_CORPUS_DIR=/workspace/netflix

python ai/scripts/chug_extract_features.py            # picks up /workspace/chug
python ai/scripts/train_chug_hdr_mos_head.py          # picks up /workspace/chug
python ai/scripts/train_konvid_mos_head.py            # picks up /workspace/konvid-150k
python ai/scripts/extract_full_features.py            # picks up /workspace/netflix
python ai/scripts/konvid_to_full_features.py          # picks up /workspace/konvid-1k
```

The env-var override does not change the per-argument flags. Every
script still accepts an explicit `--data-root <path>` / `--clips-dir
<path>` / `--scores <path>` / etc. that takes precedence over both the
env var and the default. The env var sets a new *default* that the
operator can still override per-invocation on the CLI.

## Why this exists

The audit pass that produced [ADR-0547](../adr/0547-ai-script-env-vars.md)
flagged hard-coded, maintainer-specific local corpus defaults across more than
15 scripts. ADR-1277 standardises those defaults under `.corpus/`; the env-var
layer remains the portable one-line override for containers and other hosts.

## Subprocess timeouts

Every external command an `ai/` script starts (ffprobe, ffmpeg, the `vmaf`
binary, a Python helper) runs with a time limit, so a hung process fails one
step instead of stalling a whole corpus run. Where a step was measured, its
limit is at least ten times the slowest measured run, and never under 60
seconds; the table names the measurement or says it was not measured.

| Script | Step | Constant | Value | Measured |
| --- | --- | --- | --- | --- |
| [feature_extractor.py][fe] | Feature extract | `VMAF_TIMEOUT_S` | 3600s | 229.38s (1080p, 300 frames, full features) |
| [bvi_dvc_to_full_features.py][bvi] | Header probe | `FFMPEG_TIMEOUT_S` | 60s | 0.15s (1080p) |
| [bvi_dvc_to_full_features.py][bvi] | 10-bit pipe | `TRANSCODE_TIMEOUT_S` | 300s | 10.72s (x2) |
| [bvi_dvc_to_full_features.py][bvi] | libvmaf full | `VMAF_TIMEOUT_S` | 2400s | 96.52s (x2) |
| [collect_gpu_calibration_data.py][gpu] | Calibration | `VMAF_TIMEOUT_S` | 1800s | 168.31s CPU ciede (1080p, 300 frames); GPU legs not measured |
| [export_transnet_v2.py][trans] | tf2onnx export | `SCRIPT_TIMEOUT_S` | 1800s | Not measured (tf2onnx not installed on the measuring host) |
| [extract_k150k_features.py][k150k] | Probe / decode | `FFMPEG_TIMEOUT_S` | 60s | 0.16s / 1.25s |
| [extract_k150k_features.py][k150k] | libvmaf full | `VMAF_TIMEOUT_S` | 1800s | 123.81s (2 thr) |
| [extract_konvid_frames.py][kframes] | Frame extract | `FFMPEG_TIMEOUT_S` | 60s | 0.20s (1080p) |
| [extract_ugc_features.py][ugc] | Probe / decode | `FFMPEG_TIMEOUT_S` | 60s | 0.20s / 0.29s |
| [extract_ugc_features.py][ugc] | libvmaf features | `VMAF_TIMEOUT_S` | 60s | 4.20s (360p) |
| [fetch_youtube_ugc_subset.py][fetch] | Socket operation | `API_TIMEOUT_S` | 60s | Not measured; bounds each socket operation, not a whole download |
| [konvid_to_full_features.py][kfull] | Probe / transcode | `FFMPEG_TIMEOUT_S` | 60s | 0.15s (1080p) / 0.44s (540p) |
| [konvid_to_full_features.py][kfull] | libvmaf full | `VMAF_TIMEOUT_S` | 600s | 27.56s (x1.6) |
| [konvid_to_vmaf_pairs.py][kpairs] | Probe / transcode | `FFMPEG_TIMEOUT_S` | 60s | 0.15s (1080p) / 0.44s (540p) |
| [konvid_to_vmaf_pairs.py][kpairs] | libvmaf student | `VMAF_TIMEOUT_S` | 60s | 0.39s (540p) |
| [validate_saliency_student.py][val_sal] | Registry check | `SCRIPT_TIMEOUT_S` | 60s | 0.53s |

[fe]: ../../ai/data/feature_extractor.py
[bvi]: ../../ai/scripts/bvi_dvc_to_full_features.py
[gpu]: ../../ai/scripts/collect_gpu_calibration_data.py
[trans]: ../../ai/scripts/export_transnet_v2.py
[k150k]: ../../ai/scripts/extract_k150k_features.py
[kframes]: ../../ai/scripts/extract_konvid_frames.py
[ugc]: ../../ai/scripts/extract_ugc_features.py
[fetch]: ../../ai/scripts/fetch_youtube_ugc_subset.py
[kfull]: ../../ai/scripts/konvid_to_full_features.py
[kpairs]: ../../ai/scripts/konvid_to_vmaf_pairs.py
[val_sal]: ../../ai/scripts/validate_saliency_student.py

When a command fails or runs past its limit, what happens next depends on the
script:

- The KonViD scripts log the clip as `FAILED`, the BVI-DVC script logs it as
  `FAIL`, and both continue with the rest of the batch.
- [`collect_gpu_calibration_data.py`][gpu] returns rc 124.
- [`export_transnet_v2.py`][trans] exits with the timeout message.
- A missing executable, one outside the script's allowlist, or an argument
  holding a NUL byte fails validation (`ValueError`) before any process starts.
