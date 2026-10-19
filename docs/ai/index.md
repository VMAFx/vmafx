<!-- markdownlint-disable MD013 -->
# Tiny AI

Tiny AI is the part of VMAFx that runs small ONNX perceptual-quality models next
to the classic VMAF SVM. A tiny model can replace or augment the SVM score
(full-reference), score a clip without a reference, or filter frames before
encoding. libvmaf stays C-only: models run through ONNX Runtime, and training
lives in the Python package [`ai/`](../../ai/).

## Run a shipped model

You need a libvmaf build with ONNX Runtime (`-Denable_dnn=enabled`, or `auto`
with ONNX Runtime found by `pkg-config`). `--tiny-model` takes the path of an
`.onnx` file, not a registry id. Models ship under
[`model/tiny/`](../../model/tiny/).

```bash
vmaf -r ref.yuv -d dis.yuv -w 576 -h 324 -p 420 -b 8 \
     --tiny-model model/tiny/vmaf_tiny_v2.onnx \
     --json -o scores.json
```

The tiny score is an ordinary per-frame feature in the output. It is named after
the `name` field of the model's sidecar JSON, or `vmaf_tiny_model` when the
sidecar has no `name`. Look for it in `frames[].metrics` and `pooled_metrics`
next to `vmaf`. Full details, flags and caveats are in
[inference.md](inference.md).

!!! note
    Models that read libvmaf features (the `vmaf_tiny_*` and `fr_regressor_*`
    families) make the run compute them, and their scores appear once the run
    has read its last frame. The codec-aware `fr_regressor_v2` and
    `fr_regressor_v3` also need `--tiny-codec` and `--tiny-crf`
    ([inference](inference.md#codec-aware-models)).

## Status

The shipped models were trained against the `vmaf_v0.6.1` teacher, and
[`model/tiny/registry.json`](../../model/tiny/registry.json) lists 26 entries,
13 of them smoke fixtures that exercise the loader and are not quality models.
The one-shot retrain against the `vmaf_v1.0.16_3d0h` teacher is RC9 work, see
the [roadmap](../roadmap.md) and the [retrain runbook](retrain-runbook-1246.md).
Treat accuracy numbers as pre-retrain.

## Start here

| Page | What you get |
| --- | --- |
| [Overview](overview.md) | The four capabilities, architecture and the shipped model per capability |
| [Inference](inference.md) | Run a model from the CLI, the C API or ffmpeg; device selection |
| [Training](training.md) | Train, export and register a model with `vmaf-train` |
| [Security](security.md) | Operator allowlist, size and path limits, Sigstore verification |
| [Model registry](model-registry.md) | Registry schema, sidecar fields, runtime verification |
| [Roadmap](roadmap.md) | Status of every tiny-AI item (shipped, planned, deferred) |
| [`vmaf-train` CLI](../usage/vmaf-train.md) | Reference for every `vmaf-train` subcommand |

## Models

| Page | What you get |
| --- | --- |
| [Predictor](predictor.md) | Per-codec ONNX predictors used by `vmaf-tune` |
| [Predictor v2 real-corpus training](predictor-v2-realcorpus-training.md) | Ship gate and runbook for the real-corpus predictor retrain |
| [Conformal VQA](conformal-vqa.md) | Distribution-free prediction intervals on top of any predictor (split conformal and CV+, ADR-0393) |
| [FR-from-NR adapter](fr-from-nr-adapter.md) | Use an NR model where an FR score is expected |
| [Hardware capability priors](hardware-capability-priors.md) | Per-architecture capability vectors for predictors |
| [U2NetP mirror](u2netp-mirror.md) | Hosting and licensing of the U2NetP saliency checkpoint |
| [Extractor template](extractor-template.md) | Add a tiny-AI feature extractor in C |

### Model cards

One page per shipped checkpoint. [Overview](overview.md#per-model-reference)
groups them by capability.

| Group | Cards |
| --- | --- |
| VMAF fusion (C1) | [vmaf_tiny_v2](models/vmaf_tiny_v2.md), [vmaf_tiny_v3](models/vmaf_tiny_v3.md), [vmaf_tiny_v4](models/vmaf_tiny_v4.md), [vmaf_tiny_v1](models/vmaf_tiny_v1.md), [vmaf_tiny_v1_medium](models/vmaf_tiny_v1_medium.md) |
| FR regressors (C1) | [fr_regressor_v1](models/fr_regressor_v1.md), [fr_regressor_v2](models/fr_regressor_v2.md), [fr_regressor_v2 codec-aware](models/fr_regressor_v2_codec_aware.md), [fr_regressor_v2 probabilistic](models/fr_regressor_v2_probabilistic.md), [fr_regressor_v3](models/fr_regressor_v3.md) |
| No-reference (C2) | [nr_metric_v1](models/nr_metric_v1.md), [konvid_mos_head_v1](models/konvid_mos_head_v1.md) |
| Filters (C3) | [learned_filter_v1](models/learned_filter_v1.md), [fastdvdnet_pre](models/fastdvdnet_pre.md) |
| Perceptual distance | [lpips_sq](models/lpips_sq.md), [lpips_sq_v1](models/lpips_sq_v1.md), [dists_sq](models/dists_sq.md) |
| Saliency and shots | [saliency_student_v1](models/saliency_student_v1.md), [saliency_student_v2](models/saliency_student_v2.md), [mobilesal](models/mobilesal.md), [u2netp_mirror](models/u2netp_mirror_card.md), [transnet_v2](models/transnet_v2.md) |
| CI smoke fixtures | [smoke_v0](models/smoke_v0.md), [smoke_v0_symbolic_batch](models/smoke_v0_symbolic_batch.md), [smoke_fp16_v0](models/smoke_fp16_v0.md), [smoke_multi_output_v0](models/smoke_multi_output_v0.md) |

## Quantisation

| Page | What you get |
| --- | --- |
| [Quantisation](quantization.md) | Produce and load int8 models (PTQ, QAT), gates and wire formats |
| [PTQ across execution providers](quant-eps.md) | Measured int8 PLCC drop on CPU, CUDA and OpenVINO |

## Data and corpora

| Page | What you get |
| --- | --- |
| [Training data](training-data.md) | Local Netflix corpus layout and loaders |
| [MOS corpora](mos-corpora.md) | Index of the MOS corpora, adapters and the KonViD MOS head |
| [Multi-corpus aggregation](multi-corpus-aggregation.md) | Merge corpora onto one MOS scale |
| [MOS label materializer](mos-label-materializer.md) | Join subjective labels onto feature tables |
| [Saliency feature materializer](saliency-feature-materializer.md) | Add saliency columns to feature tables |
| [Second-opinion features](second-opinion-features.md) | Join out-of-tree scorer outputs into feature tables |
| [Signal-mix audit](signal-mix-audit.md) | Coverage, redundancy and blind-spot reports for feature tables |
| [Run provenance](run-provenance.md) | The `run_provenance` block and which script writes which report |
| [Environment variables](scripts-env-vars.md) | Variables read by the `ai/` scripts |

### Corpus ingestion

| Page | Corpus |
| --- | --- |
| [KonViD-1k](konvid-1k-ingestion.md) | KonViD-1k to MOS-corpus JSONL |
| [KonViD-150k](konvid-150k-ingestion.md) | KonViD-150k to MOS-corpus JSONL |
| [K150K-A feature extraction](datasets/k150k.md) | KonViD-150k-A feature extraction |
| [LSVQ](lsvq-ingestion.md) | LSVQ to MOS-corpus JSONL |
| [YouTube UGC](youtube-ugc-ingestion.md) | YouTube UGC to MOS-corpus JSONL |
| [Waterloo IVC 4K-VQA](waterloo-ivc-4k-ingestion.md) | Waterloo IVC 4K-VQA to MOS-corpus JSONL |
| [LIVE-VQC](live-vqc-ingestion.md) | LIVE-VQC ingestion |
| [CHUG UGC-HDR](chug-ingestion.md) | Local-only CHUG HDR ingestion |
| [BVI-DVC](bvi-dvc-corpus-ingestion.md) | BVI-DVC ingestion for `fr_regressor_v2` |

## Evaluation

| Page | What you get |
| --- | --- |
| [Benchmarks](benchmarks.md) | Accuracy snapshot of shipped models and how to measure |
| [LOSO evaluation](loso-eval.md) | Leave-one-source-out harness for the tiny MLP family |
| [Bisect model quality](bisect-model-quality.md) | Find the first checkpoint that regresses (also a nightly CI gate) |
| [External benchmark wrappers](external-bench.md) | Compare fork predictors with x264-pVMAF and DOVER-Mobile |
| [CHUG HDR held-out validator](chug-hdr-held-out-validator.md) | Gate for the CHUG HDR MOS head |
| [Saliency per-block evaluation](saliency-per-mb-eval.md) | Block-level IoU for saliency masks |

## Retraining and operations

| Page | What you get |
| --- | --- |
| [v1.0.16 teacher retrain runbook](retrain-runbook-1246.md) | The one-shot retrain (epic #1246) |
| [Ensemble v2 real-corpus runbook](ensemble-v2-real-corpus-retrain-runbook.md) | Promote or hold the ensemble weights |
| [Ensemble training kit](ensemble-training-kit.md) | Portable scripts to run the ensemble pipeline on another machine |
| [Local sidecar training](local-sidecar-training.md) | Per-host residual correction for `vmaf-tune` predictors |
| [Online sidecar training](sidecar-online-training.md) | Streaming trainer service (not wired into a deployment yet) |
| [Tiny blob storage](tiny-blob-storage.md) | Where model blobs live and how the fetcher works |
| [Per-PR documentation bar](per-pr-doc-bar.md) | What a tiny-AI PR must document |
