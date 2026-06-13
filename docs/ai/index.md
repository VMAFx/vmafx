# Tiny AI

The Tiny AI surface ships small ONNX perceptual-quality models
alongside classic VMAF SVM models.

- [Overview](overview.md) — architecture, capabilities, model
  lifecycle
- [Training](training.md) — train custom models with `vmaf-train`
- [Inference](inference.md) — run models via the C API or CLI
- [Benchmarks](benchmarks.md) — latency and accuracy numbers
- [Security](security.md) — op allowlists, model validation, supply
  chain
- [Bisect model quality](bisect-model-quality.md) — binary-search a
  checkpoint timeline for the first quality regression (also wired
  as a nightly CI gate)
- [Training data](training-data.md) — Netflix corpus path
  convention, `--data-root` loader API, and evaluation harness for
  fork-local training runs
- [PTQ across EPs](quant-eps.md) — investigate int8 PLCC drop on
  CPU vs CUDA vs OpenVINO (Arc / CPU) Execution Providers
- [Conformal VQA](conformal-vqa.md) — distribution-free prediction
  intervals on top of any vmaf-tune predictor (split-conformal + CV+,
  no new dependencies, ADR-0279 implementation surface)
- [MOS-corpus ingestion family](mos-corpora.md) — unified index for
  KonViD-1k, KonViD-150k, LSVQ, YouTube UGC, Waterloo IVC 4K-VQA,
  LIVE-VQC, CHUG UGC-HDR, and BVI-DVC corpora; common schema,
  quick-start commands, aggregation workflow, and KonViD MOS head v1
  entry point
- [Signal-mix audit](signal-mix-audit.md) — table-only coverage,
  redundancy, complementary-intersection, and blind-spot reports for
  refreshed AI feature tables
- [External benchmark wrappers](external-bench.md) — wrapper-only
  comparison harness for fork predictors, x264-pVMAF, and DOVER-Mobile
- [Second-opinion features](second-opinion-features.md) — join out-of-tree
  NR/MOS scorer outputs into refreshed feature tables before retraining
- [MOS label materializer](mos-label-materializer.md) — join subjective MOS
  labels onto refreshed feature tables before real MOS-head training
- [CHUG UGC-HDR ingestion](chug-ingestion.md) — local-only CHUG
  manifest/video ingest path for HDR subjective-MOS experiments
- [KonViD MOS head v1](models/konvid_mos_head_v1.md) — 5 081-parameter
  MLP that maps canonical-6 features + saliency + shot-metadata to a
  scalar subjective MOS prediction in [1.0, 5.0] (ADR-0336)
- [Saliency per-block evaluation](saliency-per-mb-eval.md) — block-level
  IoU for saliency masks at the same granularity ROI encoders consume
- [Saliency feature materializer](saliency-feature-materializer.md) — append
  `saliency_mean` / `saliency_var` columns to existing JSONL or parquet
  feature tables before predictor and MOS-head retrains
