---
paths:
  - core/src/feature/transnet_v2.c
  - core/src/feature/transnet_v2_score.h
  - core/test/dnn/test_transnet_v2_run.c
invariant: TransNet V2 runs upstream predict_frames() windows on 0..255 thumbnails and binds its one output by position.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# TransNet V2 100-Frame Window Contract

- **`transnet_v2.c` window contract** (fork-local, ADR-0223 + ADR-0261,
  windows ADR-1527) — I/O contract `frames: float32 [1, 100, 3, 27, 48]`
  (27x48 thumbnails, 0..255) → one output `float32 [1, 100]`, bound by
  position (shipped graph calls it `output_0`). Load-bearing on rebase:
  (1) windows are upstream `predict_frames()`: window k covers frames
  `50k-25 .. 50k+74` (first frame before clip, last frame after it),
  runs when frame `50k+74` is read, and writes frames `50k .. 50k+49` from
  slots 25..74; `flush()` runs one or two windows left. Never read
  last slot as current frame's logit: it sees no later frame and misses
  cuts. (2) Samples stay in 0..255 (`luma_to_thumbnail()`; above 8 bits
  scaled by 255 / (2^bpc - 1)): upstream's ColorHistograms casts to integers
  and bins with `>> 5`. (3) `VMAF_FEATURE_EXTRACTOR_TEMPORAL` keeps
  extractor on calling thread with every frame in order; gap in
  indices is `-EINVAL`. (4) Dual feature names `shot_boundary_probability`
  and `shot_boundary` (0.5 threshold); `1.0` marks last frame of
  shot; downstream consumers bind to both. (5) shipped ONNX is real
  upstream weights (`smoke: false`, MIT, commit `a0942ca3`); NTCHW
  wrapper and `UnsortedSegmentSum` → `ScatterND` rewrite live in
  `ai/scripts/export_transnet_v2.py` and must be redone on re-export.
  `core/test/dnn/test_transnet_v2_run.c` guards (1)-(4) with shipped
  model. See
  [ADR-0223](../../../../docs/adr/0223-transnet-v2-shot-detector.md),
  [ADR-0261](../../../../docs/adr/0261-transnet-v2-real-weights.md),
  [ADR-1527](../../../../docs/adr/1527-transnet-v2-upstream-windows.md).

- **TransNet V2 shot-boundary extractor (T6-3a + T6-3a-followup,
  ADR-0223 + ADR-0261)** — second half of T6-2 bundle. Now ships
  real upstream weights via NTCHW adapter (see
  `transnet_v2.c 100-frame-window contract` invariant above).
- **TransNet V2 shot-boundary extractor (T6-3a + T6-3a-followup,
  PR #210 MERGED, ADR-0223 + ADR-0261)** — second half of T6-2
  bundle, ~1M params. DNN-backed. Ships real upstream weights via
  NTCHW adapter (see `transnet_v2.c 100-frame-window contract`
  invariant above).
