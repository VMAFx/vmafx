<!-- markdownlint-disable MD060 -->
# `vmaf-tune` AV1 encoders: libaom-av1, libsvtav1 and SVT-AV1-HDR

Use `--encoder libaom-av1` for the AV1 reference encoder (slow, best
compression) and `--encoder libsvtav1` for SVT-AV1 (fast, good for
batches). SVT-AV1-HDR is a runtime variant of `libsvtav1`, not a third
adapter. This page covers the two adapters, how they compare and how to
tune the SVT-AV1-HDR fork. The registry of all adapters is
[`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md).

## Quick start

Sweep SVT-AV1 over two presets and three CRFs:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --encoder libsvtav1 \
    --preset medium --preset slow \
    --crf 28 --crf 35 --crf 42 \
    --output corpus_av1.jsonl
```

The corpus row records the preset name (`"medium"`). The FFmpeg argv
carries SVT-AV1's integer (`-preset 7`).

## Adapter parameters

| Property | `libaom-av1` | `libsvtav1` |
|---|---|---|
| FFmpeg encoder | `libaom-av1` | `libsvtav1` |
| Quality knob | `-crf` | `-crf` |
| CRF accepted | 0..63 | 20..50 |
| Default CRF | 35 | 35 |
| Speed knob | `-cpu-used` 0..9 | `-preset` 0..13 |
| Preset names accepted | 10 (`placebo` ... `ultrafast`) | 8 (`placebo` ... `veryfast`) |
| Two-pass | yes | no (CRF mode) |
| Saliency ROI | patched `-qpfile` bridge | patched `-qpfile` bridge ([ADR-3386](../adr/3386-encoder-roi-channels-measured.md)) |
| ADR | [0279](../adr/0279-vmaf-tune-codec-adapter-libaom.md) | [0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md) |

SVT-AV1 itself accepts CRF 0..63. The adapter validates the absolute
range first and then the 20..50 window, so `--crf 10` fails with
`crf 10 outside Phase A range [20, 50]` before FFmpeg starts.

### Preset names

Both adapters accept the shared x264-style names and translate them:

| `--preset` | libaom `-cpu-used` | SVT-AV1 `-preset` |
|---|---|---|
| `placebo` | 0 | 0 |
| `slowest` | 1 | 1 |
| `slower` | 2 | 3 |
| `slow` | 3 | 5 |
| `medium` | 4 (default) | 7 (SVT-AV1 default) |
| `fast` | 5 | 9 |
| `faster` | 6 | 11 |
| `veryfast` | 7 | 13 (fastest) |
| `superfast` | 8 | not accepted |
| `ultrafast` | 9 (fastest) | not accepted |

The SVT-AV1 map is closed and order-stable
([ADR-0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md)).

The argv the adapters emit for a `medium` CRF-35 cell:

```shell
ffmpeg -i ref.y4m -c:v libaom-av1 -cpu-used 4 -crf 35 -an -y out.mkv
ffmpeg -i ref.y4m -c:v libsvtav1 -preset 7 -crf 35 -an -y out.mkv
```

## libaom vs SVT-AV1

Both target the AV1 bitstream at different points of the speed and
quality curve. Use the table as a rough guide and keep the real numbers
in your own sweep output.

| Concern | `libaom-av1` | `libsvtav1` |
|---|---|---|
| Encode wall time at a matched preset | meaningfully slower | meaningfully faster |
| Quality at slow presets, matched bitrate | slightly higher in AOM benchmarks | slightly lower |
| Quality at fast presets | comparable | comparable, sometimes ahead |
| Best fit | offline, high-quality archive encodes | live, batch, large catalogues |

Corpus rows record the `(encoder, preset, crf, vmaf_score,
encode_time_ms, bitrate_kbps)` tuple, so the predictors can pick
whichever encoder dominates the relevant region of the rate-distortion
plane for a source. To put both on one report, run
`vmaf-tune compare --encoders libaom-av1,libsvtav1`.

## SVT-AV1-HDR variant

[SVT-AV1-HDR](https://github.com/juliobbv-p/svt-av1-hdr) is a
BSD-3-Clause-Clear fork of `psy-ex/svt-av1-psy` that adds perceptual and
HDR-oriented rate-distortion features to SVT-AV1. By decision
([ADR-0644](../adr/0644-vmaf-tune-codec-runtime-variants.md)) there is no
`svtav1-hdr` adapter token (`'svtav1-hdr' in known_codecs()` is `False`),
because FFmpeg exposes the fork through the same `libsvtav1` wrapper as
mainline SVT-AV1.

Select it as the runtime variant `libsvtav1@svt-av1-hdr` and bind that
token to an FFmpeg binary linked against the fork:

```shell
vmaf-tune compare \
    --src hdr_source.mkv --width 3840 --height 2160 --pix-fmt yuv420p10le \
    --target-vmafs 94,96 \
    --encoders libsvtav1,libsvtav1@svt-av1-hdr \
    --ffmpeg-bin /opt/ffmpeg-main/bin/ffmpeg \
    --encoder-ffmpeg-bin libsvtav1@svt-av1-hdr=/opt/ffmpeg-svtav1-hdr/bin/ffmpeg \
    --json-sidecar \
    --output hdr-comparison.html
```

Mainline and the fork then appear as two curves in one report. The row
label stays readable (`codec = libsvtav1@svt-av1-hdr`) and the
provenance records `adapter = libsvtav1`, `runtime_variant =
svt-av1-hdr` and the bound `ffmpeg_bin`. Tokens without a binding use
the global `--ffmpeg-bin`. No pinned SVT-AV1-HDR container build ships
with the project; adding one is an optional follow-up that needs its own
ADR.

The variant inherits the `libsvtav1` contract: CRF 20..50 and presets
named as above. The fork itself accepts CRF `1..70` in `0.25` steps
(default `35`) and presets `-3..13` (default `4`). Those extensions are
not reachable from `vmaf-tune`, because the adapter validates before
FFmpeg is invoked.

### Passing SVT-AV1-HDR parameters

The fork's extra parameters travel as one colon-separated
`-svtav1-params key=value:key=value` string that FFmpeg's `libsvtav1`
wrapper forwards verbatim. `vmaf-tune` composes the argv as
`adapter.ffmpeg_codec_args(...)`, then `adapter.extra_params()`, then
`request.extra_params`. There are three places to inject the string:

- Python API:
  `EncodeRequest(..., extra_params=("-svtav1-params", "tune=0:cdef-scaling=12"))`.
- An adapter subclass whose `extra_params()` returns the same pair,
  registered under its own name.
- CLI: `vmaf-tune encode-profile --extra-ffmpeg-arg=-svtav1-params
  --extra-ffmpeg-arg=tune=0:cdef-scaling=12`.

`vmaf-tune compare` has no raw-argv passthrough flag. A compare sweep
runs the fork with its own defaults plus the HDR signalling below.

!!! warning "Two cautions"
    The HDR path (`vmaftune.hdr.hdr_codec_args`) already emits
    `-svtav1-params color-primaries=9:transfer-characteristics=16|18:matrix-coefficients=9:color-range=0|1`
    for `libsvtav1`, plus `mastering-display=...:content-light=...` when
    the source carries them. A second `-svtav1-params` option on the same
    command line replaces that value instead of merging, so fold your
    tuning keys into one string together with the colour keys.

The wrapper parses `-svtav1-params` last. A `crf=` or `preset=` key
inside it silently overrides the `-crf` / `-preset` the search loop
is steering, so never put those two keys in the string.

### Tuning knobs

Everything below is taken from the upstream
[README](https://github.com/juliobbv-p/svt-av1-hdr/blob/00333404f455471aaa6ee2c927cac3c93efb76e3/README.md)
and
[`Docs/Parameters.md`](https://github.com/juliobbv-p/svt-av1-hdr/blob/00333404f455471aaa6ee2c927cac3c93efb76e3/Docs/Parameters.md)
at commit `00333404f455471aaa6ee2c927cac3c93efb76e3` (2026-09-01). The
fork tracks mainline SVT-AV1 and its defaults move, so re-check those
pages before relying on a default.

| `-svtav1-params` key | Range | Default | Purpose |
|---|---|---|---|
| `tune` | `0..5` | `1` | `0` = VQ (perceptual, README recommends it with any CRF, presets 2-6), `1` = PSNR, `2` = SSIM, `3` = IQ (still images, pair with `--avif 1`), `4` = MS-SSIM, `5` = Film Grain (CRF 20-40, preset 2; equals `tune=0:enable-tf=0:enable-restoration=0:enable-cdef=0:complex-hvs=1:tx-bias=1:ac-bias=4.00`). |
| `enable-variance-boost` | `0..1` | `1` | Variance-based superblock boost (AQ modes 0 and 2); on by default in the fork, off in mainline. |
| `variance-boost-strength` | `1..4` | `2` | Boost curve strength: `1` mild, `2` gentle, `3` medium, `4` aggressive. |
| `variance-octile` | `1..8` | `5` | Selectivity: how much of a superblock must be low-variance (in eighths) before it is boosted; lower values raise bitrate. |
| `variance-boost-curve` | `0..3` | `0` | `0` default, `1` alternative, `2` still image, `3` HDR PQ curve; `3` is auto-selected when `transfer-characteristics=16` (PQ). |
| `ac-bias` | `0.0..8.0` | `1.0` | Psychovisual RD bias preserving high-frequency energy. |
| `tx-bias` | `0..3` | `0` | Sharpness-biased transform decisions: `0` off, `1` full, `2` transform size only, `3` interpolation filter only. |
| `sharp-tx` | `0..1` | `1` | Sharp transform optimisations; on by default to complement `ac-bias`, upstream recommends `0` when `ac-bias=0`. |
| `complex-hvs` | `0..1` | `0` | Highest-complexity HVS model for mode decision. |
| `qp-scale-compress-strength` | `0.0..8.0` | `1.0` | Compresses the per-temporal-layer QP range for more consistent quality (`0.0` = mainline behaviour). |
| `hbd-mds` | `0..2` | `0` | Mode-decision bit depth: `0` preset default, `1` force 10-bit, `2` adaptive 8/10-bit. |
| `cdef-scaling` | `1..30` | `15` | CDEF strength scale (`1` = 0.06x, `30` = 2x); `10..12` reported useful for sharper output. |
| `noise-adaptive-filtering` | `0..4` | `2` | Disable CDEF / restoration on detected noise: `0` off, `1` both, `2` tune default, `3` CDEF only, `4` restoration only. |
| `noise-norm-strength` | `0..4` | `1` | Boost selected AC coefficients on fine textures. |
| `tf-strength` | `0..4` | `1` | Alt-ref temporal filtering strength (each step 2x; `3` equals the mainline default). |
| `kf-tf-strength` | `0..4` | `1` | Same as `tf-strength`, keyframes only. |
| `luminance-qp-bias` | `0..100` | `0` | Frame-level QP bias from average luma (dark-scene quality). |
| `sharpness` | `-7..7` | `1` | Deblocking loop-filter sharpness and RD bias (`Docs/Parameters.md` prose still says `0`; the parameter table and README say `1`). |
| `chroma-qm-min` / `chroma-qm-max` | `0..15` | `8` / `15` | Chroma quantisation-matrix flatness bounds, decoupled from luma. |
| `max-tx-size` | `32`, `64` | `64` | Cap on transform block size. |
| `adaptive-film-grain` | `0..1` | `1` | Film-grain block size follows input resolution. |
| `alt-ssim-tuning` | `0..1` | `0` | Alternative SSIM RD path; only acts with `tune=2`. |
| `noise` | `0..200` | `0` | Synthesised film-grain table strength (`50` is roughly `--film-grain 50`). |
| `noise-chroma` | `-1..200` | `-1` | Chroma grain strength; `-1` = about 60 % of `noise`, `0` off. |
| `noise-chroma-from-luma` | `0..1` | `0` | Derive chroma grain from the luma plane (grain also on greyscale content). |
| `noise-size` | `-1..13` | `-1` | Grain particle size; `-1` = auto from resolution. |
| `dolby-vision-rpu` / `hdr10plus-json` | path | none | Dolby Vision RPU / HDR10+ JSON metadata; need the fork built with `enable-libdovi` / `enable-hdr10plus`. |

## Two-pass and saliency

- **Two-pass.** SVT-AV1 forbids multi-pass in CRF mode (checked against
  v4.1.0: `Svt[error]: CRF does not support multi-pass. Use single
  pass.`). `libsvtav1` therefore declares `supports_two_pass = False` and
  `--two-pass` runs single-pass. `libaom-av1` uses FFmpeg's `-pass` /
  `-passlogfile` and does support it. See
  [multi-pass encoding](vmaf-tune-multipass.md).
- **Saliency ROI.** `libaom-av1` uses the patched FFmpeg `-qpfile`
  bridge; `libsvtav1` also uses patched `-qpfile` with an x264-style
  16x16 macroblock file, bridged to 64x64 superblocks with at most eight
  segments ([ADR-3386](../adr/3386-encoder-roi-channels-measured.md)).
  Stock FFmpeg refuses `-qpfile`; it cannot silently drop this map. See
  [saliency-aware encoding](vmaf-tune-saliency-aware.md).
- **HDR.** Both adapters get HDR colour tags from `--auto-hdr`. See
  [HDR knobs and clip sampling](vmaf-tune-hdr-and-sampling.md).

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the
  adapter registry and contract.
- [`vmaf-tune-codec-software.md`](vmaf-tune-codec-software.md) and
  [`vmaf-tune-codec-hardware.md`](vmaf-tune-codec-hardware.md): the other
  adapter families.
- [ADR-0644](../adr/0644-vmaf-tune-codec-runtime-variants.md): runtime
  variants.
