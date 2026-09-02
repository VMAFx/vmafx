# ADR-0282: `vmaf-tune` AMD AMF codec adapters (h264 / hevc / av1)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: Lusoris
- **Tags**: tooling, ffmpeg, codec, amd, amf, vmaf-tune, fork-local

## Context

ADR-0237 set up `tools/vmaf-tune/` as a quality-aware encode
automation harness with a multi-codec adapter contract from day
one. Phase A only wired `libx264`. This ADR adds the three AMD AMF
hardware encoders (`h264_amf`, `hevc_amf`, `av1_amf`) so the harness
can drive AMD GPU encodes, which is the natural counterpart to the
NVENC and QSV adapters shipping in companion PRs.

AMF differs from NVENC / QSV / x264 in two specific ways that
shape the adapter:

1. **Coarser preset surface.** AMF exposes only three quality rungs
   — `quality`, `balanced`, `speed` — whereas the canonical preset
   vocabulary used elsewhere has seven names (`placebo` / `slowest` /
   `slower` / `slow` / `medium` / `fast` / `faster` / `veryfast` /
   `superfast` / `ultrafast`). The harness's grid generator must
   accept the 7-level vocabulary uniformly across codecs (Phase B
   bisect / Phase C predictor depend on a stable preset axis), so
   the adapter compresses 7 names into the 3 AMF rungs.
2. **AV1 is gated on RDNA3+.** `av1_amf` only registers in FFmpeg on
   Radeon RX 7000 series and newer. The runtime check is the same
   `ffmpeg -encoders` substring probe used for all three codecs;
   the documentation notes the silicon constraint so users on older
   hardware know why the adapter rejects their build.

## Decision

We will ship three AMF adapters (`h264_amf.py`, `hevc_amf.py`,
`av1_amf.py`) sharing a common `_amf_common.py` base. The base
implements the 7-into-3 preset compression as a fixed table, the
`-rc cqp` rate-control argv block, and an `ensure_amf_available`
probe; concrete subclasses pin only the `encoder` and `name`
fields. The seven canonical preset names map deterministically onto
the three AMF rungs:

| Preset names | AMF `-quality` |
| --- | --- |
| `placebo` / `slowest` / `slower` / `slow` | `quality` |
| `medium` (default) | `balanced` |
| `fast` / `faster` / `veryfast` / `superfast` / `ultrafast` | `speed` |

The QP range is the (15, 40) Phase A window so AMF results plot
on the same axis as `libx264` CRF results in Phase B / C.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| One file per codec, no shared base | Trivial to read in isolation. | ~3x duplication of preset table, probe, argv block; preset compression has to be kept in sync three ways. | Rejected — diff against NVENC / QSV would have made every fix a triple edit. |
| Single `amf.py` selecting codec via constructor arg | Less file count. | Breaks the "one-file-per-codec" pattern set by `x264.py` and required by ADR-0237's adapter-registry sketch; registry lookup keys go off-axis. | Rejected — registry is keyed by `encoder` name, one file per encoder is the established shape. |
| Map presets onto AMF integers (`-quality 0..2`) instead of names | Slightly shorter argv. | FFmpeg AMF accepts named values directly; the integer form is undocumented and less stable across AMF versions. | Rejected — name form is explicit and survives AMF updates. |
| Expose all 7 preset names without compression (error on the 4 that don't map) | Forces the user to think about AMF granularity. | Defeats the cross-codec uniform preset axis Phase B / C depend on. | Rejected — the harness must accept the canonical 7 names for every codec. |

## Consequences

- **Positive.** AMD-GPU users can drive `vmaf-tune` against the
  same corpus shape as software-encoded runs. The shared base
  keeps the three AMF files at ~30 LOC each; future AMF tweaks
  (e.g. AMF v2 quality rungs) are a single-file change.
- **Positive.** The 7-into-3 preset compression is documented
  inline so a Phase B / C consumer reading a corpus row can
  reconstruct exactly which AMF rung produced it from the
  `preset` field.
- **Negative.** Multiple preset names produce identical encodes
  (e.g. `slow`, `slower`, `slowest`, `placebo` all produce the
  same `quality`-rung output). The corpus row preserves the
  original preset name, so analytics that group by preset will
  see the granularity loss; per ADR-0237 the canonical
  cross-codec axis is `(preset, qp)` and the consumer must dedupe
  on `(preset, qp, encoder, source)` if needed.
- **Neutral.** No runtime AMF probe at adapter-construction
  time: `ensure_amf_available` is exposed as a standalone helper
  that callers (or Phase B / C) invoke once per corpus run. The
  unit tests cover both branches via a mocked subprocess runner.

## References

- Parent: [ADR-0237](0237-quality-aware-encode-automation.md) —
  `vmaf-tune` umbrella spec.
- Companion PRs (NVENC, QSV adapters) — running in parallel with
  this PR.
- Source: `req` — direct user task brief (2026-05-03):
  *"Add AMD AMF hardware encoder adapters (`h264_amf`, `hevc_amf`,
  `av1_amf`) to vmaf-tune. Companion to the NVENC + QSV adapter PRs
  running in parallel."*
- Source: `req` — paraphrased: AMF preset granularity is coarser
  than NVENC / QSV — only three quality levels — and the seven
  canonical preset names compress to those three rungs.
