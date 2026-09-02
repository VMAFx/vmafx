- **`vmaf-tune` ProRes VideoToolbox codec adapter (extends [ADR-0283](docs/adr/0283-vmaf-tune-videotoolbox-adapters.md)).**
  Completes the macOS hardware-encoder coverage trio alongside the
  H.264 and HEVC VideoToolbox adapters. New
  `tools/vmaf-tune/src/vmaftune/codec_adapters/prores_videotoolbox.py`
  declares `ProresVideoToolboxAdapter` — same registry shape, same
  preset → `-realtime` mapping, but the harness's `--crf` slot now
  carries the integer ProRes **tier id** (0=`proxy` → 5=`xq`)
  instead of a `-q:v` value: ProRes is a fixed-rate intermediate
  codec, quality is selected entirely by tier. Profile values
  (`proxy`/`lt`/`standard`/`hq`/`4444`/`xq`) verified against
  FFmpeg n8.1.1 `libavcodec/videotoolboxenc.c` `prores_options`
  AVOption table. Registered under `prores_videotoolbox` in
  `codec_adapters/__init__.py`. 22 new subprocess-mocked smoke
  tests under `tools/vmaf-tune/tests/test_codec_adapter_prores_videotoolbox.py`
  cover per-tier argv emission, validate bounds, the
  `prores_profile_name` round trip, and the encode-driver shape.
  No `ENCODER_VOCAB` change — the proxy fast path raises
  `ProxyError` for ProRes until a future v4 retrain expands the
  vocab; the live-encode loop works unchanged. Hardware
  availability: M1 Pro / Max / Ultra and later (Intel + T2 Macs do
  **not** have the ProRes hardware block).
