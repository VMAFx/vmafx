# vmaf-tune corner cases: parse_versions libaom/vvenc + compare --preset default (ADR-1077)

- `parse_versions` now handles `libaom-av1` and `libvvenc` encoders: returns
  a parsed version string when the banner is present, or the stable adapter
  name (`"libaom-av1"` / `"libvvenc"`) when absent — previously returned
  `"unknown"` for both, breaking corpus-row encoder-version conditioning.
- `_VERSION_PROBE_PATTERNS` extended with `--enable-libaom` and
  `--enable-libvvenc` so `probe_encoder_info` correctly sets
  `codec_detected=True` for these codecs.
- `compare --preset` now defaults to `"medium"` instead of `None`; the
  previous `None` default caused `_encode_and_score` to reject every encode
  with `preset not in adapter.presets`, producing all-failed CRF-sweep rows
  with exit code 0.
