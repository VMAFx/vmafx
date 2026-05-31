<!-- markdownlint-disable MD013 -->
# Research-0074: `vmaf-tune` Apple VideoToolbox codec adapters

- **Date**: 2026-05-05
- **Status**: Implementation digest (companion to ADR-0283).
- **Tags**: tooling, ffmpeg, codec, hardware-encoder, apple, fork-local
- **Companion ADR**: [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md)

## Question

Add Apple VideoToolbox H.264 + HEVC adapters to `tools/vmaf-tune/`
following the same one-file-per-codec contract NVENC / AMF / QSV
already use. Goal: macOS-host coverage for the harness so corpus
runs on Apple Silicon are first-class.

## Findings

1. **Encoder names.** FFmpeg exposes two VideoToolbox-backed encoders:
   `h264_videotoolbox` and `hevc_videotoolbox`. AV1 is not yet
   surfaced (no `av1_videotoolbox` as of FFmpeg 8.1 / 2026-Q1) — Apple
   Silicon does not include an AV1 hardware encoder block.
2. **Quality knob.** Both encoders take `-q:v <int>` on the
   `[0, 100]` scale where higher is better quality. This inverts the
   x264 / x265 / NVENC convention (`crf` lower = better) — the
   adapter's `invert_quality=False` flag tells the harness to leave
   the value alone. Default is `60` (matches FFmpeg's documented
   sweet spot for Apple-Silicon HEVC encoding).
3. **Preset axis.** VideoToolbox exposes only a binary `-realtime
   {0,1}` flag. The harness's nine-name preset taxonomy
   (`ultrafast` … `veryslow`) collapses onto that boolean per the
   table in `_videotoolbox_common.py`:
   `ultrafast`/`superfast`/`veryfast`/`faster`/`fast` → `realtime=1`;
   `medium`/`slow`/`slower`/`veryslow` → `realtime=0`. The mapping is
   intentionally lossy — VT cannot expose a finer dial.
4. **Subprocess seam.** The smoke test mocks `subprocess.run` so
   Linux CI stays green without a macOS runner. End-to-end run is
   left to a macOS contributor with VideoToolbox available locally.
5. **Codec-aware regressor coupling.** Originally proposed alongside
   a 16-slot codec one-hot vocab expansion. After `fr_regressor_v2`
   shipped to production with the 13-slot vocab v2 schema (ADR-0291
   / PR #397), the schema-expansion side requires a fresh production
   retrain to clear the 0.95 LOSO PLCC ship gate (ADR-0235); that
   work is split out into a follow-up PR. The VT adapters land
   independently because the registry-add doesn't change any
   numeric output.

## Implementation note

Three new files:

- `tools/vmaf-tune/src/vmaftune/codec_adapters/h264_videotoolbox.py`
- `tools/vmaf-tune/src/vmaftune/codec_adapters/hevc_videotoolbox.py`
- `tools/vmaf-tune/src/vmaftune/codec_adapters/_videotoolbox_common.py`

Plus a registry entry in `codec_adapters/__init__.py` and the smoke
suite at `tests/test_codec_adapter_videotoolbox.py` (9 cases).

## Decision

Land the two adapters now along the same one-file-per-codec contract
NVENC / AMF / QSV use. Defer the codec-vocab schema-expansion + the
`fr_regressor_v2_hw` retrain to a follow-up PR.

## References

- [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) — this PR.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — `vmaf-tune` umbrella.
- [ADR-0235](../adr/0235-codec-aware-fr-regressor.md) — codec-aware FR ship gate (relevant to the deferred companion).
- [ADR-0291](../adr/0291-fr-regressor-v2-prod-loso.md) — fr_regressor_v2 production checkpoint.
