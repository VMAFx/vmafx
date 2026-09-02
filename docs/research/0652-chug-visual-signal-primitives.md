# Research-0652: CHUG Visual-Signal Primitives

## Question

Which unblocked signal gap from the fresh signal-mix audit can be closed
without waiting for a trained HDR model or a new public dataset?

## Findings

- CHUG feature extraction already decodes matched reference and distorted
  clips to aligned `yuv420p10le` files before running libvmaf.
- The CHUG MOS head needs row-local cues for blur/noise/grain experiments.
  Canonical six libvmaf features are not explicit about those axes.
- A small sampled luma pass can produce deterministic proxies:
  luma standard deviation, Laplacian variance, neighboring-pixel
  high-frequency energy, and robust Laplacian MAD.
- These primitives are cheap enough to compute during materialisation and
  can be cached next to the existing libvmaf feature cache.

## Decision Input

Add `feature_ref_*`, `feature_dis_*`, and `feature_delta_*` columns for:

- `luma_std`
- `sharpness_laplacian_var`
- `highfreq_abs_mean`
- `noise_lap_mad`

The names are deliberately descriptive rather than claiming a full
perceptual model. In particular, high Laplacian variance may represent
detail or noise; downstream audits must inspect it jointly with MOS and
other features.

## Validation Plan

- Unit-test the YUV10 reader and signal calculator against a synthetic
  checker pattern.
- Unit-test CHUG materialisation so the emitted row includes reference,
  distorted, and delta visual fields.
- Keep the real ffmpeg/vmaf degraded-pair smoke as a separate optional
  command, with `VMAF_BIN_FOR_TESTS` pinned to the known-good build when
  the stale host binary stalls.
