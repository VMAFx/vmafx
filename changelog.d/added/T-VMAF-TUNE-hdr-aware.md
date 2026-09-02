- **`vmaf-tune` HDR-aware encoding + scoring (ADR-0300, Bucket #9 of
  the PR #354 capability audit).** New `vmaftune.hdr` module exposes
  `detect_hdr` (ffprobe-driven PQ / HLG classification with strict
  BT.2020 primaries gate so malformed signaling falls back to SDR),
  `hdr_codec_args` (per-encoder dispatch table covering `libx264`,
  `libx265`, `libsvtav1`, `hevc_nvenc`, `libvvenc`), and
  `select_hdr_vmaf_model` (returns `model/vmaf_hdr_*.json` if shipped).
  Corpus driver gains `--auto-hdr` / `--force-sdr` / `--force-hdr-pq` /
  `--force-hdr-hlg` mutually-exclusive modes and three new schema-v2
  row keys (`hdr_transfer`, `hdr_primaries`, `hdr_forced`);
  `SCHEMA_VERSION` bumped 1 → 2. `vmaf --model` arg now accepts
  pre-formatted `path=` / `version=` strings so an HDR-trained model
  flows through unchanged. Encode-side correctness ships now; the
  HDR-VMAF model port (Netflix's `vmaf_hdr_v0.6.1.json`) is filed as
  a backlog follow-up — until it lands, HDR sources are scored against
  the SDR model with a one-shot warning. Adds 21 mocked tests under
  `tools/vmaf-tune/tests/test_hdr.py` covering detection of SDR / PQ /
  HLG / mismatched-primaries / missing-file / ffprobe-failure /
  invalid-JSON, codec dispatch shape per encoder, and end-to-end
  corpus integration with `force-hdr-pq` / `force-sdr`. User docs:
  [`docs/usage/vmaf-tune.md` § HDR-aware tuning](../docs/usage/vmaf-tune.md).
