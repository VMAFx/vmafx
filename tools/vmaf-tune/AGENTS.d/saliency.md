---
paths:
  - tools/vmaf-tune/src/vmaftune/saliency.py
  - tools/vmaf-tune/tests/test_saliency*.py
invariant: Saliency inference consumes RGB; predictor uses raw-YUV helper; temporal aggregation is CLI contract.
---
<!-- markdownlint-disable MD024 -->
# Saliency integration and aggregation

- **Saliency inference consumes RGB, not luma-replicated input
  (ADR-0430).** `saliency.compute_saliency_map()` reads yuv420p
  Y/U/V, nearest-neighbour upsamples chroma, converts BT.709
  limited-range YUV to RGB, and only then applies ImageNet
  normalisation for `saliency_student_v1`. Do not reintroduce old
  luma-only tensor path unless model card and operator docs
  explicitly change.
- **Predictor saliency uses raw-YUV saliency helper (ADR-0654).**
  `predictor_features._compute_saliency()` must decode requested
  shot range to temporary `yuv420p` before calling
  `saliency.compute_saliency_map(raw_path, width, height, ...)`;
  public `predict --source` accepts containers, but saliency
  helper intentionally remains raw-YUV-only.
  `vmaf_train.predictor_train.project_row()` must preserve row-provided
  saliency / signalstats values in existing 14-column predictor
  layout and only zero-fill missing legacy rows.
- **Saliency temporal aggregation is CLI-visible contract
  (ADR-0396 Phase 1).** `recommend-saliency --saliency-aggregator`
  exposes `mean`, `ema`, `max`, and `motion-weighted`. `mean` is
  compatibility default; changing that default or removing reducer
  changes user-visible encode behaviour and needs same-PR
  usage-doc update plus ADR-0396 follow-up.
- **Saliency signal blend matches `vmaf-roi` (ADR-0293).**
  `saliency.py` deliberately mirrors `vmaf-roi`'s ADR-0247 signal
  blend (`offset = (2*sal − 1) * foreground_offset`, clamped to
  ±12). If `vmaf-roi`'s C-side blend changes, `saliency.py`
  follows in same PR — bit-for-bit equivalence is pinned by
  `tests/test_saliency.py` and is contract that lets us swap
  Python implementation for `vmaf-roi` shell-out later without
  behaviour drift. ONNX session is second test seam
  (`session_factory` parameter) — production callers leave it
  default; tests inject fake. Do not import `onnxruntime` at
  module top-level; lazy-load via `_import_onnxruntime` so corpus
  subcommand and unit tests work without it installed.
- **Any frame height is valid; no `height % 8` guard (ADR-1540 follow-up).**
  `compute_saliency_map()` and `pkg/saliency.ComputeMap()` zero-pad
  tensor to multiple of 32 and crop map back, which covers
  student's three stride-2 stages for every size (measured down to 1x1
  with `saliency_student_v1.onnx`). Do not reintroduce divisibility
  check; `tests/test_saliency.py::test_compute_saliency_map_height_not_multiple_of_8`
  and Go `TestComputeMap` subtest of same name pin it.
