- **Real `two_pass_args` for all 14 codec adapters that previously raised
  `NotImplementedError`** (ADR-0546). Software adapters `libaom-av1`
  and `libvvenc` now run true two-invocation 2-pass via FFmpeg's
  generic `-pass N -passlogfile <prefix>` pair (joining `libx264` /
  `libx265` / `libvpx-vp9`). `libsvtav1` returns VBR-mode argv but
  keeps `supports_two_pass = False` because SVT-AV1 forbids multi-pass
  in CRF mode (the harness default). NVENC / QSV / AMF adapters return
  their single-invocation analysis flags (`-multipass fullres`,
  `-extbrc 1 -look_ahead_depth 40`, `-preanalysis true`) — callers
  splice the pass-1 argv into `EncodeRequest.extra_params` for a
  quality-boosted single-pass encode. All four VideoToolbox adapters
  raise the new `VideoToolboxTwoPassUnsupportedError` (a typed
  `NotImplementedError` subclass) documenting that
  `VTCompressionSession` has no multi-pass C API.
- **`tools/vmaf-tune/tests/test_codec_adapter_two_pass_real.py`** —
  74 cases covering per-codec pass-1 / pass-2 / pass-0 / out-of-range
  argv shapes, `supports_two_pass` flag values, and the typed
  VideoToolbox refusal.
- **`docs/usage/vmaf-tune.md`** — refreshed Phase F codec support
  matrix with per-codec `two_pass_args(1, _)` return values, the
  hardware quality-boost composition pattern, and the SvtAv1
  CRF-mode prohibition.
