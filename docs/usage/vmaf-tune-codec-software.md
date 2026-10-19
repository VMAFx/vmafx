<!-- markdownlint-disable MD060 -->
# `vmaf-tune` software encoders: x264, x265, VP9 and VVenC

Use these four CPU adapters when you want the best quality per bit and
can spend the encode time. This page covers `libx264`, `libx265`,
`libvpx-vp9` and `libvvenc`. The AV1 software encoders have their own
page, [`vmaf-tune-codec-av1.md`](vmaf-tune-codec-av1.md), and the
registry of all adapters is
[`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md).

All software adapters route through the same codec-adapter registry. The
search loops never branch on the codec name.

## Quick start

```shell
vmaf-tune corpus \
    --encoder libx265 \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --preset medium --preset slow --preset placebo \
    --crf 23 --crf 28 --crf 34 \
    --output corpus_x265.jsonl
```

Replace `--encoder` with `libx264`, `libvpx-vp9` or `libvvenc` and pick
presets and quality values from the table below.

## Adapter parameters

| Property | `libx264` | `libx265` | `libvpx-vp9` | `libvvenc` |
|---|---|---|---|---|
| Codec | H.264 | HEVC | VP9 | VVC (H.266) |
| Quality knob | `-crf` | `-crf` | `-crf` | `-qp` |
| Accepted range | 0..51 | 15..40 | 0..63 | 17..50 |
| Default | 23 | 28 | 32 | 32 |
| Preset names | `ultrafast` ... `veryslow` (9) | `ultrafast` ... `veryslow`, `placebo` (10) | 10 names | 10 names |
| Native speed knob | `-preset` | `-preset` | `-deadline good -cpu-used 0..5` | `-preset` (5 levels) |
| Two-pass | yes | yes | yes | yes |
| Stats capture | yes | yes | no | no |
| Saliency ROI | `-qpfile` (patched FFmpeg) | `-x265-params zones=` | no | `-vvenc-params ROIFile=` |

The range is what the adapter accepts; a value outside it raises
`ValueError` before FFmpeg starts. The x265 and VVenC windows are the
informative windows, narrower than the encoders' own scales (0..51 and
0..63).

## libx264

The reference adapter
([ADR-0237](../adr/0237-quality-aware-encode-automation.md)).
It emits `-c:v libx264 -preset <name> -crf <q>`. In a two-pass encode it
omits `-crf`, since a CRF flag conflicts with pass-based rate control.
Its pass-1 text stats file is parsed for per-frame encoder statistics.

## libx265

x265 ships ten presets (`ultrafast` ... `placebo`) on the same 0..51 CRF
scale as x264. The adapter routes `--encoder libx265` through FFmpeg's
`-c:v libx265` path, so the FFmpeg build needs `--enable-libx265`
([ADR-0288](../adr/0288-vmaf-tune-codec-adapter-x265.md)).

Set `--pix-fmt yuv420p10le` for the 10-bit pipeline. The adapter reports
the matching HEVC profile through `X265Adapter.profile_for(pix_fmt)`, for
downstream consumers that need it:

| `--pix-fmt` | HEVC profile |
|---|---|
| `yuv420p` | `main` |
| `yuv422p` | `main422-8` |
| `yuv444p` | `main444-8` |
| `yuv420p10le` | `main10` |
| `yuv422p10le` | `main422-10` |
| `yuv444p10le` | `main444-10` |
| `yuv420p12le` | `main12` |

An unlisted pixel format falls back to `main`. For HDR sources the
`--auto-hdr` flags add the PQ or HLG colour parameters; see
[HDR knobs and clip sampling](vmaf-tune-hdr-and-sampling.md).

## libvpx-vp9

The VP9 adapter routes `--encoder libvpx-vp9` through FFmpeg's
`libvpx-vp9` wrapper. It maps the shared presets to
`-deadline good -cpu-used N`, emits `-crf`, and adds `-b:v 0`, which
forces VP9 constant-quality mode (without it FFmpeg treats `-crf` as a
constrained-quality hint). It also adds `-row-mt 1` for row
multithreading.

| `--preset` | `-cpu-used` |
|---|---|
| `placebo`, `slowest` | 0 |
| `slower` | 1 |
| `slow` | 2 |
| `medium` | 3 |
| `fast` | 4 |
| `faster`, `veryfast`, `superfast`, `ultrafast` | 5 |

```shell
vmaf-tune corpus \
    --encoder libvpx-vp9 \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --preset medium --preset fast \
    --crf 28 --crf 32 --crf 36 \
    --output corpus_vp9.jsonl
```

`--two-pass` works for VP9 through FFmpeg's generic `-pass` /
`-passlogfile` switches. The per-frame encoder-stats columns stay zero
until a parser for libvpx's binary first-pass data lands.

## libvvenc (H.266 / VVC)

`libvvenc` drives Fraunhofer HHI's open-source VVC encoder through
FFmpeg's `-c:v libvvenc` wrapper
([ADR-0285](../adr/0285-vmaf-tune-vvenc-nnvc.md)). VVC is the ITU-T and
ISO standard that succeeds HEVC and delivers roughly 30 to 50 % better
compression at equal quality. As a rule of thumb, VVenC `slow` is about
5 to 10 % better than HEVC `slower` at the same bitrate and 3 to 5 times
slower in wall-clock time. It is the adapter for "longer encodes for
tighter bitrates".

### Quality knob and presets

| Property | Value |
|---|---|
| Quality knob | `qp`, carried by `--crf` (the wrapper accepts the value whatever the label) |
| Accepted range | 17..50 (full VVenC scale is 0..63) |
| Default | 32 |
| Native presets | `faster`, `fast`, `medium`, `slow`, `slower` |

The adapter accepts the ten shared preset names and compresses them onto
the five native levels with a static map, in line with the other
adapters so predictor inputs stay codec-uniform:

| `--preset` | VVenC preset |
|---|---|
| `placebo`, `slowest`, `slower` | `slower` |
| `slow` | `slow` |
| `medium` | `medium` |
| `fast` | `fast` |
| `faster`, `veryfast`, `superfast`, `ultrafast` | `faster` |

### Tuning surface (VVenC 1.14.0 knobs)

The adapter exposes a curated subset of VVenC's configuration through
FFmpeg's `-vvenc-params key=value:key=value` channel. The keys come from
[`source/Lib/apputils/VVEncAppCfg.h`](https://github.com/fraunhoferhhi/vvenc/blob/v1.14.0/source/Lib/apputils/VVEncAppCfg.h)
at tag `v1.14.0` (SHA `9428ea8636ae7f443ecde89999d16b2dfc421524`,
accessed 2026-05-09).

The knobs are fields of the Python `VVenCAdapter` dataclass. They have no
CLI flag, so a `corpus` run uses the library defaults. Every knob
defaults to `None`, which preserves the library default; the search loop
opts into a value only when the corpus row records it.

| Field | VVenC key | Default | Effect |
|---|---|---|---|
| `perceptual_qpa` | `PerceptQPA` | library default | XPSNR-driven perceptual QP adaptation. It shifts the rate-distortion curve and is recorded per row in `encoder_extra_params`. |
| `internal_bitdepth` | `InternalBitDepth` | library default (10 for VVC) | Force 8 or 10-bit internal precision; needed for HDR profiles. |
| `tier` | `Tier` | library default (`main`) | `main` or `high`; caps the signalled maximum bitrate and resolution. |
| `tiles` | `Tiles` | single tile | `(cols, rows)`, emitted as `NxM`; parallel encode of high-resolution content. |
| `max_parallel_frames` | `MaxParallelFrames` | library default (auto) | `0` disables, `>=2` enables parallel frames. |
| `rpr` | `RPR` | library default | Reference-picture resampling: `0` off, `1` on, `2` RPR-ready. |
| `sao` | `SAO` | library default (on) | Sample Adaptive Offset loop filter, for ablation studies. |
| `alf` | `ALF` | library default | Adaptive Loop Filter, for ablation studies. |
| `ccalf` | `CCALF` | library default | Cross-Component ALF; meaningful only when `alf` is on. |

Toggles are emitted in field-declaration order so the argv stays
byte-stable for cache-key hashing
([ADR-0298](../adr/0298-vmaf-tune-cache.md)). `adapter_version` is `"2"`
for the 2026-05-09 surface, so stale cached results are invalidated.

### NN-VC status (deferred)

VVC the standard defines neural-network tool points (intra prediction,
loop filter, super-resolution), but VVenC 1.14.0 ships none of them. An
earlier draft of this adapter exposed an `nnvc_intra` toggle that emitted
`-vvenc-params IntraNN=1`. That key has never existed in any released
VVenC, and the toggle was removed (ADR-0285, status update 2026-05-09).
If upstream VVenC adds NN-VC tools, the adapter can pick them up through
the placeholder pattern of
[ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md).

### External binary requirements

Running the adapter end to end needs:

- `ffmpeg` built with `--enable-libvvenc`, on `PATH` or given with
  `--ffmpeg-bin`.
- The `libvvenc` shared library and headers from
  <https://github.com/fraunhoferhhi/vvenc>.

The shipped unit tests mock `subprocess.run`, so the adapter can be
exercised with neither present. Integration smoke runs on a CI runner
that has a `libvvenc`-enabled FFmpeg.

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the
  adapter registry and contract.
- [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md): `--two-pass` per
  adapter.
- [`vmaf-tune-saliency-aware.md`](vmaf-tune-saliency-aware.md): ROI
  encodes with x264, x265 and VVenC.
