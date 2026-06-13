# ADR-0281: `vmaf-tune` Intel QSV codec adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, qsv, intel, fork-local

## Context

[ADR-0237](0237-quality-aware-encode-automation.md) Phase A pinned
the codec-adapter contract under
`tools/vmaf-tune/src/vmaftune/codec_adapters/` with the explicit
expectation that "new codecs are one-file additions" off the same
registry. That adapter contract was deliberately multi-codec from
day one — Phase B (target-VMAF bisect) and Phase C (per-title CRF
predictor) need a representative spread of codecs in the corpus,
not just the libx264 baseline.

The fork has no Intel hardware-encode coverage in the harness yet.
QSV is the only path to drive the integrated GPU on consumer Intel
silicon (Kaby Lake and newer) and the Arc / Battlemage discrete
parts; without it the fork can corpus-train CPU encodes only and the
`vmaf_tiny` codec-bucket one-hot (ADR-0235) keeps the QSV column
empty. NVENC and AMF land in parallel sibling PRs; this ADR scopes
the QSV slice.

The three QSV encoders (`h264_qsv`, `hevc_qsv`, `av1_qsv`) share the
same FFmpeg parameter shape — seven preset levels with x264-style
names (`veryslow` through `veryfast`, no `ultrafast` /
`superfast`), one quality knob (`-global_quality N`, ICQ rate
control, integer range `1..51`). The shared shape invites a
factored common module rather than three independent adapter files
each repeating the preset list and the validation helpers.

## Decision

We will ship three new codec adapters under
`tools/vmaf-tune/src/vmaftune/codec_adapters/` — `h264_qsv.py`,
`hevc_qsv.py`, `av1_qsv.py` — each backed by a shared private
module `_qsv_common.py` that pins the QSV preset vocabulary, the
ICQ quality range, the preset identity check (QSV uses x264 names
verbatim, so the "translation" is a guarded identity), the
`global_quality` validator, and an `ffmpeg -encoders` probe that
fails fast when libmfx / VPL is not compiled into the FFmpeg on
`PATH`. Each adapter is a thin frozen dataclass that only pins
`name` / `encoder` and delegates `validate()` to the shared helpers.
The registry entry for all three goes into
`codec_adapters/__init__.py`; the search loop continues to route
through the registry without branching on codec identity.

The encode pipeline (`encode.py`) is currently x264-CRF-tied and
will be widened in a later PR to dispatch on
`adapter.quality_knob`. That widening is explicitly out of scope
here — the adapter classes pin the contract and the
parameter-validation surface so the encode-side wiring lands as a
follow-up without requiring further adapter changes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Three independent adapter files, no shared module | Trivial; matches the libx264 single-file shape | Triplicates the preset list, the ICQ window constant, and the validate() body; drift waiting to happen | QSV's three encoders share *exactly* the same parameter shape — the duplication carries no information |
| One unified `qsv.py` adapter parametrised by FFmpeg encoder name | Maximally compact | Breaks the registry-as-flat-table invariant from ADR-0237 (each adapter is one file, one class); makes per-codec quirks (e.g. AV1's narrower hardware availability) harder to attach later | Future per-codec divergence (10-bit support flags, 4:2:2 chroma support, B-frame handling, codec-specific extra params) is more naturally hung off three classes than off one parametric one |
| Skip the FFmpeg probe; let `ffmpeg` emit "Unknown encoder" at run time | Less code | Hides the libmfx / VPL build-flag mismatch behind a generic FFmpeg error buried in stderr; harness behaviour is "encode failed for unclear reasons" | The probe is ~20 LOC and the diagnostic is the difference between "rebuild FFmpeg" and "is your driver wedged" |
| Use VBR / CQP instead of ICQ for the quality knob | VBR maps cleanly to bitrate budgets used in Phase E | ICQ is the QSV-native perceptual-quality knob; CRF-style search spaces are what the corpus is structured around; libmfx documents ICQ as the recommended single-pass quality mode | ICQ is the equivalent of CRF for QSV; using anything else would force the harness to translate quality knobs per codec |

## Consequences

- **Positive**: corpus generation can target Intel hardware encode for
  H.264 / HEVC / AV1 from the same harness; codec-bucket one-hot
  (ADR-0235) gains real samples in the QSV slot; per-title CRF
  predictor (Phase C) trains on a representative codec spread; the
  shared `_qsv_common.py` is the natural seam for any future
  QSV-specific quirks (chroma support flags, low-power vs full
  encode mode).
- **Negative**: the encode pipeline still hardcodes `-crf` and will
  not yet successfully drive a QSV encode end-to-end — that wiring
  is a follow-up. The adapter classes are valid but inert until
  the search loop dispatches on `adapter.quality_knob`. The doc
  flags this clearly so users do not file "QSV doesn't work" issues
  against the adapter PR.
- **Neutral / follow-ups**: an encode-pipeline widening PR
  (separate ADR) flips `build_ffmpeg_command` to consult
  `adapter.quality_knob` and emit `-global_quality` instead of
  `-crf` for QSV; the same widening unlocks NVENC / AMF (sibling
  PRs) and HEVC / SVT-AV1 / VP9 / VVenC (later PRs).

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md)
  (`vmaf-tune` umbrella).
- Companion: [ADR-0235](0235-codec-collision-bucket.md) (codec
  one-hot consumes the QSV bucket).
- Sibling PRs (parallel work): NVENC adapter ADR (forthcoming),
  AMF adapter ADR (forthcoming).
- FFmpeg QSV docs:
  [`ffmpeg-qsv.html`](https://ffmpeg.org/ffmpeg-codecs.html#QSV-encoders).
- libmfx ICQ rate control:
  Intel Media SDK Developer's Guide, `MFX_RATECONTROL_ICQ` mode.
- Source: `req` ("Add Intel QSV (Quick Sync Video) hardware encoder
  adapters (`h264_qsv`, `hevc_qsv`, `av1_qsv`) to `vmaf-tune`.
  Companion to the NVENC + AMF adapter PRs running in parallel.").

### Status update 2026-05-08: install discoverability backfill

Per ADR-0028 (immutable-once-Accepted bodies — appendix only),
recording a follow-up landing in a separate doc-only PR.

The SYCL audit (research-0086, Topic C / issue #464) found that
none of the six per-OS install pages
(`docs/getting-started/install/{arch,fedora,ubuntu,macos,windows}.md`)
referenced the runtime dependency these adapters require. The
adapters' own runtime probe
(`_qsv_common.ffmpeg_supports_encoder`) was already correct and
covered both `libmfx` and `libvpl`; the gap was discoverability —
a user landing on the install page from `mkdocs` did not learn
that `h264_qsv` / `hevc_qsv` / `av1_qsv` need an FFmpeg built
against `libvpl` (or, for legacy FFmpeg < n6.0, against the now
archived Media SDK `libmfx`).

The backfill ships a per-OS QSV section with verified package
names (Arch `libvpl` + `vpl-gpu-rt`; Fedora `libvpl` +
`libvpl-tools`; Ubuntu `libvpl2` + `libvpl-dev`), an explicit
"unsupported" note on macOS, an Intel-driver-bundle pointer on
Windows, and a hardware-capability matrix mapping Intel CPU /
GPU generations to which of the three QSV codecs they actually
support. No adapter code changed in the backfill.
