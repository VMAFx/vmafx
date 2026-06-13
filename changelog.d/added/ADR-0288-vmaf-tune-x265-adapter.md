- **`vmaf-tune` libx265 codec adapter
  ([ADR-0288](docs/adr/0288-vmaf-tune-codec-adapter-x265.md)).** First
  sibling codec after the [ADR-0237](docs/adr/0237-quality-aware-encode-automation.md)
  Phase A `libx264` scaffold. New
  `tools/vmaf-tune/src/vmaftune/codec_adapters/x265.py` declares the
  `X265Adapter` (10 presets including `placebo`, 0..51 CRF window pinned
  to the same Phase A informative range as x264, `profile_for(pix_fmt)`
  helper that maps `yuv420p10le` → `main10` for downstream HDR work).
  Registered under `libx265` in
  `codec_adapters/__init__.py`; `--encoder` CLI flag now accepts
  `libx264 | libx265`. `encode.parse_versions` gains an encoder-aware
  banner regex so corpus rows record `libx265-<version>` correctly.
  No `SCHEMA_VERSION` bump — the existing `encoder` row column already
  carries codec identity. 14 new subprocess-mocked smoke tests under
  `tools/vmaf-tune/tests/test_codec_adapter_x265.py`; real-binary
  integration test gated on `VMAF_TUNE_INTEGRATION=1`. Unblocks
  ADR-0235 codec-aware FR regressor and PR #354 audit's buckets #6
  (bitrate-ladder), #7 (codec-comparison), #9 (HDR), #15 (Pareto).
