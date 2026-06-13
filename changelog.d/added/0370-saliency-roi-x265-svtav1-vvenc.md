- **`vmaf-tune` saliency-aware ROI for x265 / SVT-AV1 / libvvenc
  (ADR-0370, re-port of closed PR #456).** Extends
  `vmaftune.saliency.saliency_aware_encode` from x264-only to all four
  software encoder targets by adding a codec-dispatch table
  (`_SALIENCY_DISPATCH`) that routes the pixel-level QP-offset map to the
  correct per-encoder ROI channel:
  - **libx265** — `-x265-params zones=0,N,q=<delta>` (per-clip spatial
    mean; ADR-0370 documents why temporal zones are a follow-up).
  - **libsvtav1** — `-svtav1-params qp-file=<path>` (64×64 super-block
    granularity; format: space-separated rows, blank-line frame separator,
    per SVT-AV1 v2.1.0 `EbAppConfig.c`).
  - **libvvenc** — `-vvenc-params ROIFile=<path>` (64×64 CTU granularity;
    format: comma-separated rows, per VVenC v1.14.0 `VVEncAppCfg.h`).
  Each encoder receives the raw per-pixel `int32 [H, W]` map from
  `saliency_to_qp_map` and reduces it to its own native granularity —
  no double-reduction. The `--two-pass` / `--with-uncertainty` /
  `--calibration-sidecar` surfaces (F.3/F.5/conformal) are unchanged.
  Also fixes a CLI bug in `_run_recommend_saliency` where
  `SaliencyConfig(qp_offset=…)` should have been
  `SaliencyConfig(foreground_offset=…)`.
  11 new end-to-end dispatch tests under
  [`tools/vmaf-tune/tests/test_saliency_roi_codec.py`](../tools/vmaf-tune/tests/test_saliency_roi_codec.py)
  cover per-adapter argv shape, granularity, cross-contamination guards,
  and the unknown-encoder graceful-fallback path.
