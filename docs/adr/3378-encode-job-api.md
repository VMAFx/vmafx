<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-3378: Encode-job API: one video stream in, a verified video stream out, auto first

- **Status**: Superseded by [ADR-3382](3382-encode-service-leaves-vmafx.md)
- **Date**: 2026-10-26
- **Deciders**: Lusoris
- **Tags**: api, vmaf-tune, platform, encode, hdr

## Context

VMAFx already encodes: `vmaf-tune` drives FFmpeg through software and hardware
codec adapters ([ADR-0237](0237-quality-aware-encode-automation.md),
[ADR-0297](0297-vmaf-tune-encode-multi-codec.md)), picks clips
([ADR-0301](0301-vmaf-tune-sample-clip.md)) and has a proposed `auto` mode
([ADR-0397](0397-vmaf-tune-phase-f-auto.md)). The controller
([ADR-0711](0711-vmafx-controller-impl.md)) runs scoring jobs only, and its
definition in `api/vmafx-platform.toml` has no encode job.

A media manager that consumes VMAFx (SnatchArr) needs to hand over the video
stream of a file and get back a re-encoded video stream it can trust. The
split was agreed on 2026-10-26: VMAFx owns everything about the video stream
(codec choice, hardware probe and fallback, HDR10, HDR10+ and Dolby Vision
metadata, scaling, deinterlacing, quality search, GPU and CPU pools); the
consumer owns rules, stream selection, remux, audio, staging, verification of
the container and notifications. The requirements for each area were answered
the same day (see References).

A gap list against those requirements was drawn up and judged on 2026-10-26
(11 claims spot-checked, one corrected). It finds no encode job in the API,
and gaps in every area:

- **Codecs**: no `librav1e` adapter; `auto` does not rank AV1 encoders
  (`tools/vmaf-tune/src/vmaftune/auto.py`); `libvvenc` is offered by default
  (`cli.py`, `auto.py`).
- **Hardware**: the node probe parses `ffmpeg -encoders` only and runs no test
  encode (`cmd/vmafx-node/probe/probe.go`); there is no software-only,
  hardware-preferred or hardware-only mode.
- **Quality budget**: only an absolute bitrate cap (`--max-budget-bitrate`);
  no size cap relative to the source, no "keep the original unless smaller"
  rule; fixed CRF and bitrate are still exposed by other subcommands.
- **HDR**: HDR10 static metadata only (`hdr.py`); no HDR10+, no Dolby Vision
  profile detection, no `dovi_tool`, no profile 5 guard, no opt-in tone map.
- **Geometry**: the ladder can upscale (`auto.py`); no maximum-resolution
  downscale; no frame-rate passthrough in `encode.py` or
  `pkg/encoder/encoder.go`; no field-order detection or deinterlace.
- **Pools**: the dynamic backend pool serves scoring only; there are no
  bounded encode slots and no GPU lease.

## Decision

We will add an **encode job** to the VMAFx platform API: input is a file path
plus the index of one video stream, output is a video-only file plus the
provenance record of [ADR-2073](2073-vmafx-provenance-record.md) extended with
the encode decisions. The job carries a quality goal, not encoder settings:
a target score, a size cap relative to the source, and the allowed codecs.
VMAFx chooses codec, encoder, settings, resolution and pool. If the best
output is not smaller than the source, the job returns "keep original" and no
file. No fixed CRF or bitrate is accepted on the job.

The work lands in phased slices, each one milestone issue with a planted
test that fails on the code before the slice:

| Phase | Slice | Contents | Planted test (fails before the slice) |
|---|---|---|---|
| 1 | `S-api-encode-job` | `EncodeJob` / `EncodeParams` / `EncodeResult` in `api/vmafx-platform.toml`, controller `SubmitJob` kind, generated bindings, input contract (file + stream index), video-only output + provenance | a submit with an audio stream index, or a fixed CRF, is refused; a valid job returns a result whose provenance names stream index and encoder |
| 2 | `S-auto-quality-budget` | `auto` as the only mode: target score, relative size cap, allowed codecs; keep-original verdict | a source whose best encode is larger returns keep-original and writes no file |
| 3 | `S-codec-rav1e-tiering` | AV1 primary `libsvtav1`, advanced `libaom-av1` / `librav1e`; HEVC `libx265`; H.264 `libx264`; VVC `libvvenc` experimental and off unless enabled | `auto` with defaults never selects `libvvenc`; a job allowing only AV1 selects `libsvtav1` first |
| 4 | `S-hw-probe-modes` | worker-start probe with a short synthetic encode per hardware encoder; per-job mode software-only, hardware-preferred (CPU fallback named in the result), hardware-only (queue and wait, never a silent CPU encode) | an encoder listed by `-encoders` whose test encode fails is not advertised; a hardware-only job with no hardware stays queued |
| 5 | `S-pool-bounded-slots` | bounded pools `remux_io`, `encode_cpu`, `encode_gpu`; GPU taken from the dynamic backend pool through a lease, never an ordinal pin; remux never starved | with `encode_cpu` full, a remux-class task still starts; no job names a device ordinal |
| 6 | `S-geom-deinterlace-fps` | never upscale; optional maximum resolution downscale; frame-rate passthrough; deinterlace only sources whose field order says interlaced; no auto-crop | a 720p source with a 1080p rung yields no 1080p candidate; a progressive source gets no deinterlace filter |
| 7 | `S-hdr-dovi-hdr10plus` | keep HDR10; HDR10+ round trip through `hdr10plus_tool`; Dolby Vision 8.1 keeps the RPU, profile 7 converted to 8.1 with `dovi_tool`, profile 5 not re-encoded unless reshaping is configured; tone map to SDR only when the job asks | a profile 5 source returns skip; an HDR10+ source's output carries the same dynamic metadata count |
| 8 | later slices | scoring, compare, ladder, per-shot, saliency and sample clips exposed through the same job surface; `vmafx-node` runs encode jobs | each later issue names its own planted test |

Phase 1 and 2 come first because every later slice is a choice inside `auto`;
no slice adds a manual encoder-settings path to the job.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Encode job with a quality goal, auto only (chosen) | One contract for every consumer; VMAFx can change encoders without breaking callers; keep-original rule protects the library | Callers cannot force a setting | Matches the agreed split: the consumer owns rules, VMAFx owns the stream |
| Expose `vmaf-tune` subcommands with CRF / bitrate over the API | Already exists; full control | Every consumer re-implements codec choice; fixed settings bypass the quality goal | The requirement is auto only, no fixed CRF or bitrate |
| Consumer runs FFmpeg itself and calls VMAFx only to score | Smallest VMAFx change | HDR metadata, hardware fallback and pools duplicated in each consumer | Two implementations of one behaviour (HISS-19) |
| One ADR per area, no umbrella | Smaller records | No single order; slices conflict on the job shape | The job shape must be fixed once, before the slices |

## Consequences

- **Positive**: one versioned job contract; every encode decision lands in the
  provenance record; hardware and pool behaviour become testable.
- **Negative**: eight milestones of work; `librav1e`, `hdr10plus_tool` and
  `dovi_tool` become runtime dependencies of the node image (each added by
  its slice with its own supply-chain note).
- **Neutral / follow-ups**: each slice ships its docs page under
  `docs/usage/` or `docs/server/`, its changelog fragment and its planted
  test; [ADR-0397](0397-vmaf-tune-phase-f-auto.md) stays the record of the
  `auto` search itself, and this ADR adds the job surface around it.

## References

- Gap list (private evidence, judged 2026-10-26): `.workingdir/evidence/encode-job/encode-gap-list-20261026.md`.
- Ownership split between VMAFx and SnatchArr, decision ledger id `snatcharr-vmafx-video-split-20261026` (2026-10-26, paraphrased: VMAFx owns the whole video stream).
- Full toolkit scope, ledger id `snatcharr-vmafx-full-toolkit-20261026` (2026-10-26, paraphrased: the job surface covers all of VMAFx, auto first).
- Requirement answers oq-8 (codecs), oq-9 (hardware), oq-10 (quality), oq-11 (HDR), oq-12 (geometry), oq-17 (pools), 2026-10-26 (paraphrased in the Decision table).
