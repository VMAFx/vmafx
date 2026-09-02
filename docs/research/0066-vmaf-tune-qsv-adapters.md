# Research Digest 0054 — `vmaf-tune` Intel QSV codec adapters

- **Date**: 2026-05-03
- **Author**: Lusoris
- **Status**: Companion to [ADR-0281](../adr/0281-vmaf-tune-qsv-adapters.md)
- **Tags**: tooling, ffmpeg, codec, qsv, intel, fork-local

## Question

`vmaf-tune` Phase A wired only `libx264`; ADR-0237 explicitly
deferred multi-codec support to "one-file additions" off the
codec-adapter registry. The fork's first hardware-encode coverage
needs to land. Three questions framed the ADR:

1. **Which QSV encoders does FFmpeg expose, and what is their
   parameter shape?** The harness needs a quality knob, a preset
   vocabulary, and a way to detect a build that lacks libmfx /
   VPL.
2. **Are the three QSV encoders (`h264_qsv`, `hevc_qsv`,
   `av1_qsv`) similar enough to share a common module, or do
   their parameter shapes diverge enough to keep them
   independent?**
3. **Which rate-control mode is the right perceptual-quality
   knob for corpus generation — ICQ, VBR, or CQP?**

## Findings

### FFmpeg QSV encoder surface

The FFmpeg QSV bridge (driven by libmfx on older silicon, VPL on
Tiger Lake and newer) exposes three video encoders the harness
cares about:

| FFmpeg encoder | Codec | Hardware | Notes |
| --- | --- | --- | --- |
| `h264_qsv` | H.264 / AVC | Intel iGPU 7th-gen+ (Kaby Lake) or Arc / Battlemage | Most broadly available. |
| `hevc_qsv` | HEVC / H.265 | Intel iGPU 7th-gen+; 10-bit needs Tiger Lake / 11th-gen+ | Main10 profile gated on hardware. |
| `av1_qsv` | AV1 | Intel iGPU 12th-gen+ only or Arc / Battlemage | Fixed-function AV1 encode block. |

Older silicon registers `av1_qsv` if FFmpeg is built with VPL but
fails at runtime with a libmfx `MFX_ERR_UNSUPPORTED` style
diagnostic. The harness probe (`ffmpeg -encoders`) catches the
build-time mismatch but not the runtime hardware mismatch — the
adapter doc flags this explicitly.

### Parameter shape

All three encoders share the same shape:

- **Preset vocabulary**: `veryslow, slower, slow, medium, fast,
  faster, veryfast` — seven levels, identical names to x264's
  medium-and-down subset (no `ultrafast` or `superfast`). The
  FFmpeg QSV bridge accepts the seven names verbatim, so the
  preset "translation" is a guarded identity rather than a
  lookup table.
- **Quality knob**: `-global_quality N` selects the libmfx
  `MFX_RATECONTROL_ICQ` mode. Integer range `1..51`, where `1`
  is highest quality and `51` is lowest — semantically similar
  to x264's CRF (lower is better). Values outside the window
  are clipped by libmfx; the harness rejects them up front so
  corpus rows stay reproducible.
- **Output**: standard FFmpeg muxers; nothing QSV-specific in
  the muxer surface.

### Rate-control mode trade-off

Three modes were viable:

| Mode | Knob | Corpus fit | Decision |
| --- | --- | --- | --- |
| ICQ (Intelligent Constant Quality) | `-global_quality N` | Single-pass perceptual quality target — direct CRF analogue | **Picked.** ICQ is libmfx's documented recommended single-pass perceptual-quality mode. |
| CQP (Constant QP) | `-q:v N` | Predictable bitrate at the cost of perceptual quality at scene cuts | Rejected — CQP doesn't track perceptual-quality budgets across content variety. |
| VBR (Variable Bit Rate) | `-b:v N` | Bitrate-budget-targeted; not perceptual-quality-targeted | Rejected for corpus generation. Useful in Phase E (Pareto ABR ladder) but the corpus harness is structured around CRF-style search. |

ICQ is the only knob that lets a `(codec, preset, quality)` cell
mean the same thing across `libx264` and the QSV encoders. The
NVENC adapter PR uses NVENC's `-cq N` for symmetric reasons; AMF
uses `-quality balanced -rc cqp`.

### Shared-module trade-off

The `_qsv_common.py` shared module collects:

- Preset tuple constant (`QSV_PRESETS`).
- ICQ window constant (`QSV_QUALITY_RANGE = (1, 51)`,
  `QSV_QUALITY_DEFAULT = 23`).
- Identity preset validator (`preset_to_qsv`).
- Range validator (`validate_global_quality`).
- `ffmpeg -encoders` probe (`ffmpeg_supports_encoder`).
- `RuntimeError`-raising probe wrapper (`require_qsv_encoder`).

The three adapter classes (`H264QsvAdapter`, `HevcQsvAdapter`,
`Av1QsvAdapter`) are 30-line frozen dataclasses each — only
`name` / `encoder` differ between them.

This factoring is a deliberate exception to the "one file per
codec, no shared code" Phase A convention. The justification is
that the three QSV encoders share *exactly* the same parameter
shape — duplicating the preset list and the validators across
three files would carry no information and would invite drift.
The three adapter files keep the registry-as-flat-table
invariant; the shared module is private (`_qsv_common`) and not
exposed through the package `__all__`.

## Out-of-scope

- The encode pipeline (`encode.py`) currently hardcodes `-crf`
  and will not yet successfully drive a QSV encode end-to-end.
  Widening it to dispatch on `adapter.quality_knob` is a
  separate PR that also unlocks NVENC / AMF / HEVC / SVT-AV1.
- 10-bit Main10 HEVC support, 4:2:2 chroma, B-frame depth
  controls, low-power vs full encode mode — all of these are
  per-codec quirks that hang off the adapter classes when the
  encode pipeline grows the corresponding optional knobs.
- Runtime hardware-mismatch detection (e.g. `av1_qsv` registered
  but failing at startup on a 10th-gen iGPU) — a deferred
  follow-up; the build-time probe shipped here covers the more
  common "FFmpeg without libmfx / VPL" failure mode.

## References

- FFmpeg QSV codec docs: <https://ffmpeg.org/ffmpeg-codecs.html#QSV-encoders>.
- Intel Media SDK Developer's Guide, `MFX_RATECONTROL_ICQ`.
- Parent ADR: [ADR-0237](../adr/0237-quality-aware-encode-automation.md).
- Companion: [ADR-0235](../adr/0235-codec-collision-bucket.md)
  (codec one-hot consumes the QSV bucket).
