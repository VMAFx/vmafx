<!-- markdownlint-disable MD060 -->
# Research-0087: `vmaf-tune` codec-adapter dispatcher pivot (HP-1 audit)

- **Date**: 2026-05-08
- **Authors**: Lusoris
- **Companion ADR**: [ADR-0326](../adr/0326-vmaftune-codec-adapter-runtime-contract.md)

## Summary

Phase A of `vmaf-tune` shipped sixteen codec adapters with a Protocol
that promised a per-codec ``ffmpeg_codec_args(preset, quality)`` slice.
The Phase A audit (HP-1) found the contract was docstring-only — every
live argv composition site bypassed it and emitted the libx264-shaped
hardcode ``-c:v <enc> -preset <p> -crf <q>``. This research digest
records the per-codec FFmpeg flag shapes that motivated the dispatcher
pivot in the HP-1 PR (see ADR-0326).

## Per-codec FFmpeg argv shapes

The hardcoded shape works for libx264 (and accidentally for libx265,
which uses the same `-preset` + `-crf` knobs). Every other codec in the
registry diverges; Table 1 records the canonical FFmpeg flags per
codec, sourced from upstream `ffmpeg -h encoder=<name>` listings and
the FFmpeg source tree.

### Table 1 — Codec → FFmpeg argv shape

| Codec               | Preset flag       | Preset shape         | Quality flag      | Quality shape | Extra flags                       |
|---------------------|-------------------|----------------------|-------------------|---------------|-----------------------------------|
| libx264             | `-preset`         | mnemonic (medium...) | `-crf`            | 0..51         | —                                 |
| libx265             | `-preset`         | mnemonic + placebo   | `-crf`            | 0..51         | —                                 |
| libaom-av1          | none              | `-cpu-used` 0..9     | `-crf`            | 0..63         | `-cpu-used` is mandatory          |
| libsvtav1           | `-preset`         | integer 0..13        | `-crf`            | 0..63         | —                                 |
| libvvenc            | `-preset`         | 5-level native       | `-qp`             | 0..63         | optional `-vvenc-params`          |
| h264_nvenc          | `-preset`         | `pN` (p1..p7)        | `-cq`             | 0..51         | —                                 |
| hevc_nvenc          | `-preset`         | `pN`                 | `-cq`             | 0..51         | —                                 |
| av1_nvenc           | `-preset`         | `pN`                 | `-cq`             | 0..51         | Ada+ HW only                      |
| h264_amf            | none              | (3-level `-quality`) | `-qp_i` + `-qp_p` | 0..51         | `-quality {speed,balanced,quality}`, `-rc cqp` |
| hevc_amf            | none              | (3-level `-quality`) | `-qp_i` + `-qp_p` | 0..51         | as h264_amf                       |
| av1_amf             | none              | (3-level `-quality`) | `-qp_i` + `-qp_p` | 0..51         | RDNA3+ HW only                    |
| h264_qsv            | `-preset`         | mnemonic subset      | `-global_quality` | 1..51         | (ICQ rate control)                |
| hevc_qsv            | `-preset`         | mnemonic subset      | `-global_quality` | 1..51         | (ICQ rate control)                |
| av1_qsv             | `-preset`         | mnemonic subset      | `-global_quality` | 1..51         | 12th-gen+ Intel HW                |
| h264_videotoolbox   | none              | `-realtime` 0/1      | `-q:v`            | 0..100        | `-realtime` is mandatory          |
| hevc_videotoolbox   | none              | `-realtime` 0/1      | `-q:v`            | 0..100        | `-realtime` is mandatory          |

### Table 2 — What the legacy hardcode emitted vs. what each codec needs

| Codec               | Legacy hardcode                                  | Codec-correct shape (post-HP-1) | Pre-HP-1 effect at run time |
|---------------------|--------------------------------------------------|---------------------------------|------------------------------|
| libx264             | `-c:v libx264 -preset medium -crf 23`            | `-c:v libx264 -preset medium -crf 23` | Correct                      |
| libx265             | `-c:v libx265 -preset medium -crf 28`            | `-c:v libx265 -preset medium -crf 28` | Correct                      |
| libaom-av1          | `-c:v libaom-av1 -preset medium -crf 35`         | `-c:v libaom-av1 -cpu-used 4 -crf 35` | **Crash**: libaom-av1 has no `-preset` flag |
| libsvtav1           | `-c:v libsvtav1 -preset medium -crf 35`          | `-c:v libsvtav1 -preset 7 -crf 35` | Encoder rejects "medium"; FFmpeg falls back to default |
| libvvenc            | `-c:v libvvenc -preset medium -crf 32`           | `-c:v libvvenc -preset medium -qp 32` | `-crf` is silently ignored; QP 0 is used |
| h264_nvenc          | `-c:v h264_nvenc -preset medium -crf 23`         | `-c:v h264_nvenc -preset p4 -cq 23` | "medium" → undefined; `-crf` silently dropped |
| hevc_nvenc          | as above                                         | `-c:v hevc_nvenc -preset p4 -cq 23` | as above                     |
| av1_nvenc           | as above                                         | `-c:v av1_nvenc -preset p4 -cq 23` | as above                     |
| h264_amf            | `-c:v h264_amf -preset medium -crf 23`           | `-c:v h264_amf -quality balanced -rc cqp -qp_i 23 -qp_p 23` | `-preset` is ignored; rate control falls back to VBR default; `-crf` silently dropped |
| hevc_amf            | as above                                         | as above                        | as above                     |
| av1_amf             | as above                                         | as above                        | as above                     |
| h264_qsv            | `-c:v h264_qsv -preset medium -crf 23`           | `-c:v h264_qsv -preset medium -global_quality 23` | `-crf` is silently dropped; ICQ disabled |
| hevc_qsv            | as above                                         | as above                        | as above                     |
| av1_qsv             | as above                                         | as above                        | as above                     |
| h264_videotoolbox   | `-c:v h264_videotoolbox -preset medium -crf 23`  | `-c:v h264_videotoolbox -realtime 0 -q:v 65` | `-preset` ignored; `-crf` silently dropped; encoder runs at default quality |
| hevc_videotoolbox   | as above                                         | as above                        | as above                     |

## Per-adapter `ffmpeg_codec_args` audit (pre-HP-1)

| Adapter                | `ffmpeg_codec_args` shipped? | Notes                                                        |
|------------------------|------------------------------|--------------------------------------------------------------|
| `X264Adapter`          | Yes                          | Returns the legacy shape verbatim                            |
| `X265Adapter`          | **No**                       | Adapter only carried metadata + `validate` + `probe_args`    |
| `LibaomAdapter`        | Yes (non-conforming)         | Returned a tuple without `-c:v`, with trailing `-an`         |
| `SvtAv1Adapter`        | **No**                       | —                                                            |
| `VVenCAdapter`         | **No**                       | `extra_params` existed for NNVC toggles only                 |
| `H264NvencAdapter`     | **No**                       | `nvenc_preset` helper existed but was unwired                |
| `HevcNvencAdapter`     | **No**                       |                                                              |
| `Av1NvencAdapter`      | **No**                       |                                                              |
| `H264AMFAdapter`       | **No**                       | `_AMFAdapterBase.extra_params(preset, qp)` had wrong signature |
| `HEVCAMFAdapter`       | **No**                       |                                                              |
| `AV1AMFAdapter`        | **No**                       |                                                              |
| `H264QsvAdapter`       | **No**                       |                                                              |
| `HevcQsvAdapter`       | **No**                       |                                                              |
| `Av1QsvAdapter`        | **No**                       |                                                              |
| `H264VideoToolboxAdapter` | Yes                       | Implemented through `_PRESET_TO_REALTIME` table              |
| `HEVCVideoToolboxAdapter` | Yes                       |                                                              |

Eleven of sixteen adapters did not ship `ffmpeg_codec_args`. The
predictor probe-encode path (`_gop_common.probe_args`) had its own
hardcoded slice and so was the only live consumer of the codec-correct
flags pre-HP-1.

## Hardcode call sites replaced

| File                                                          | Function                  | Pre-HP-1 shape                                                       |
|---------------------------------------------------------------|---------------------------|----------------------------------------------------------------------|
| `tools/vmaf-tune/src/vmaftune/encode.py`                      | `build_ffmpeg_command`    | `["-c:v", req.encoder, "-preset", req.preset, "-crf", str(req.crf)]` |
| `tools/vmaf-tune/src/vmaftune/per_shot.py`                    | `_segment_command`        | Same (preset omitted; only `-c:v <enc> -crf <crf>`)                  |
| `tools/vmaf-tune/src/vmaftune/corpus.py`                      | `iter_rows`               | Composes `EncodeRequest` and calls `run_encode` → routes through `build_ffmpeg_command` |

## Smoke test methodology

`tests/test_encode_dispatcher_per_adapter.py` parametrises across every
entry in `codec_adapters._REGISTRY`. For each adapter it:

1. Builds an `EncodeRequest` with a known-good `(preset, quality)` pair
   inside the adapter's declared `quality_range`.
2. Calls `build_ffmpeg_command(req)` and asserts the codec-correct
   flag tokens are present in the composed argv.
3. Calls `run_encode(req, runner=fake_runner)` with a stub that
   captures the argv before the would-be subprocess call; asserts the
   same codec-correct tokens reach the subprocess boundary.

Two pinning tests (`test_x264_argv_byte_for_byte_legacy_shape`,
`test_x265_argv_byte_for_byte_legacy_shape`) assert the codecs that
already worked pre-HP-1 produce **byte-for-byte identical** argv
post-pivot. A regression test
(`test_libaom_argv_does_not_contain_preset_flag`) explicitly defends
the libaom case that would have crashed FFmpeg pre-HP-1.

A meta-test (`test_fixture_table_covers_every_registered_adapter`)
fails if a new adapter lands in `_REGISTRY` without a corresponding
fixture row, keeping the smoke surface in lock-step with the registry.

## Out of scope for HP-1

- `parse_versions(stderr, encoder=...)` per-codec banner detection
  (referenced in `tests/test_encode_multi_codec.py`).
- `run_encode(encoder_runner=...)` kwarg alias for the subprocess
  runner (also referenced in the same test file).
- Auto-splicing `adapter.extra_params()` into the composed argv
  (currently the harness only consumes `EncodeRequest.extra_params`).

These are tracked separately; they don't affect HP-1's "make the 11
broken adapters functional" target.
