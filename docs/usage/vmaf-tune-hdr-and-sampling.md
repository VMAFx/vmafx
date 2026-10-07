<!-- markdownlint-disable MD060 -->
# `vmaf-tune` HDR knobs and clip sampling

Two independent `vmaf-tune corpus` features shape what each run encodes
and scores:

- **HDR detection and forcing** ([ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md))
  adds PQ or HLG colour flags to the encode when the source is HDR, with
  manual overrides.
- **Clip sampling** ([ADR-0301](../adr/0301-vmaf-tune-sample-clip.md),
  [ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md)) scores only the
  centre slice of each source, trading a small VMAF difference for a
  near-linear wall-time win.

The base tool is [`vmaf-tune.md`](vmaf-tune.md).

## Quick start

Auto-detect HDR and sweep one source:

```shell
vmaf-tune corpus \
    --source hdr_pq.mp4 \
    --width 3840 --height 2160 --pix-fmt yuv420p10le \
    --framerate 24 \
    --encoder hevc_nvenc --preset medium --crf 22 --crf 26 \
    --output hdr_corpus.jsonl
```

`--auto-hdr` is the default, so the `hevc_nvenc` adapter receives the PQ
flags without further options. Add `--duration 60 --sample-clip-seconds
10` to score only a 10-second slice (see [Clip sampling](#clip-sampling)).

## HDR mode

### How a source is classified

With `--auto-hdr`, the corpus runner runs `ffprobe` once per source
(`--ffprobe-bin` selects another binary) and looks at the first video
stream. The source is HDR only if both conditions hold:

- `color_transfer` is `smpte2084` (PQ) or `arib-std-b67` / `hlg`.
- `color_primaries` is one of `bt2020`, `bt2020nc`, `bt2020-ncl`,
  `bt2020c`, `bt2020-cl`.

A mismatch, such as PQ transfer with BT.709 primaries, is treated as SDR
with a warning, because mislabelling SDR as HDR is the dangerous failure.
A missing file, a missing `ffprobe` or a probe failure also means SDR.
Mastering-display and content-light SEI side data are read when present
and passed to the encoders that take them (x265, SVT-AV1, HEVC NVENC).

For an HDR source the runner appends the encoder's HDR flag set to each
encode (see the table below). The corpus row records `hdr_transfer`,
`hdr_primaries` and `hdr_forced`.

### Overrides

The four flags are mutually exclusive:

| Flag | Effect |
|---|---|
| `--auto-hdr` | Default. Detect through ffprobe; inject HDR flags only when signalled. |
| `--force-sdr` | Treat every source as SDR; skip detection and flag injection. |
| `--force-hdr-pq` | Treat every source as HDR PQ (SMPTE-2084), whatever the probe says. |
| `--force-hdr-hlg` | Treat every source as HDR HLG (ARIB STD-B67), whatever the probe says. |

Forcing helps when the source is HDR but has lost its signalling, for
example raw YUV with no container, because auto-detection cannot probe a
raw file. It also lets you score an SDR source through the HDR pipeline
as a deliberate comparison baseline. A forced HDR run has no mastering
metadata to inject, because nothing can be probed.

### Per-encoder HDR flags

HDR flag dispatch is centralised in `vmaftune.hdr.hdr_codec_args()`.

| Encoder | HDR flags added |
|---|---|
| `libx264` | Global `-color_*` tags only. x264 has no in-stream HDR SEI; use x265 or SVT-AV1 for real HDR encodes. |
| `libaom-av1` | Global `-color_*` tags only. |
| `libx265` | Global `-color_*` plus `-x265-params colorprim=bt2020:transfer=...:colormatrix=bt2020nc:range=...`, with `master-display`, `max-cll` and `hdr10-opt=1` (PQ) when available. |
| `libsvtav1` | Global `-color_*` plus `-svtav1-params color-primaries=9:transfer-characteristics=16` (PQ) or `=18` (HLG), `:matrix-coefficients=9:color-range=...`, and `mastering-display` / `content-light` when available. |
| `libvvenc` | Global `-color_*` tags only; SEI options live behind `-vvenc-params` in newer FFmpeg builds. |
| `hevc_nvenc` | `-pix_fmt p010le -profile:v main10` plus global `-color_*`. FFmpeg has no `-master_display` / `-max_cll` option for NVENC (it aborts the encode); the static HDR metadata reaches NVENC as side data of decoded frames, which a container source carries and a raw YUV does not. |
| `hevc_qsv`, `hevc_amf`, `hevc_videotoolbox` | `-pix_fmt p010le -profile:v main10` plus global `-color_*`. |
| `av1_nvenc`, `av1_qsv`, `av1_amf` | `-pix_fmt p010le` plus global `-color_*`. |

An encoder that is not in the table (for example `h264_nvenc`,
`libvpx-vp9` or `av1_videotoolbox`) gets no HDR flags, and a warning is
logged. The row's `hdr_*` fields still record the detection result.

### HDR-VMAF scoring is deferred

!!! warning "HDR sources are scored with the SDR model"
    `vmaftune.hdr.select_hdr_vmaf_model()` looks for an HDR-trained model
    in `model/`: first `vmaf_hdr_v0.6.1.json`, then any
    `vmaf_hdr_*.json`. The project ships none (`model/` holds only
    `vmaf_hdr_model_card.md`), so HDR sources are scored against the SDR
    model. A one-time warning is logged per corpus run. The resulting
    `vmaf_score` values trend low for high-luminance regions and are not
    comparable with SDR scores.

Netflix publishes the HDR model in a research bundle outside its public
repository, and a project-local licence review gates shipping it
([ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md)). Drop a licensed copy at
`model/vmaf_hdr_v0.6.1.json` and the harness picks it up with no code
change.

## Clip sampling

Set `--sample-clip-seconds N` to encode and score only the centre
`N`-second slice of each source instead of the whole reference. The slice
length is measured against `--duration`, the source length in seconds, so
pass both:

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 60 \
    --encoder libx264 --preset medium --preset slow \
    --crf 22 --crf 28 --crf 34 \
    --sample-clip-seconds 10 \
    --output corpus.jsonl
```

| Value | Meaning |
|---|---|
| `0` (default) | Score the full source. Most accurate, slowest. |
| `N > 0` | Score the centre `N`-second slice of each source. |

`corpus`, `compare`, `auto` and `encode-profile` accept the flag. The
`encode-profile` subcommand also has `--sample-clip-start-s` to place the
slice.

### How the slice is chosen

- **Centred.** The slice is `(duration - N) / 2` to `(duration + N) / 2`.
  A 60 s source with `N = 10` yields seconds 25.0 to 35.0.
- **Scoring.** libvmaf reads only the matching reference window, through
  `--frame_skip_ref` and `--frame_cnt`.
- **Fallback.** If `N` is not shorter than `--duration`, or `--duration`
  is `0`, the full source is used and the row is tagged
  `clip_mode="full"`. This is not an error and logs nothing.
- **Row fields.** A sampled row carries `clip_mode="sample_10s"` (the
  rounded seconds). `bitrate_kbps` is computed against the encoded
  duration, so sampled rows are not biased low; `duration_s` keeps the
  source provenance. Downstream code can filter sampled rows out, weight
  them differently, or rescore the chosen cell on the full source.

### When to sample

- **Fast iteration** while developing a corpus method. A 60 s source
  sampled at 10 s gives roughly a 6x wall-clock speedup, because encode
  time scales with the slice length.
- **Pre-flight smoke test** before a multi-hour sweep.

The accuracy cost depends on content. Expect a 1 to 2 VMAF-point
difference on diverse content (mixed-shot trailers, sports, action), and
about 0.3 to 0.5 points on uniform content (single-shot interviews,
animation, static scenes). These are typical-case figures from
[ADR-0301](../adr/0301-vmaf-tune-sample-clip.md), so check them on your
own corpus. The difference is consistent per cell, so the ordering of
`(preset, crf)` cells survives, which is what the target-VMAF bisect and
the per-title CRF predictor consume. Rescore the predictor's pick on the
full source as a last step.

### When not to sample

- **Production verdicts.** A final ladder, per-title CRF or
  promote-or-hold decision should use the full source.
- **Short sources.** A 10 s slice of a 12 s source is most of the source
  and only adds a ragged measurement.
- **Very high-motion content.** The centre slice may not represent the
  temporal distribution. Per-shot scoring
  ([`vmaf-perShot.md`](vmaf-perShot.md)) is the right tool there.

Shot-aware slice placement (for example from `transnet_v2`) is on the
follow-up backlog; today's placement is the centre only.

## Combined example

A pre-flight smoke run: one HDR source, scored on CUDA, 10-second centre
clip. The corpus runs one encoder, so this makes 2 cells (1 preset times
2 CRFs):

```shell
vmaf-tune corpus \
    --source hdr_test.mp4 \
    --width 3840 --height 2160 --pix-fmt yuv420p10le --framerate 24 \
    --duration 60 \
    --encoder hevc_nvenc \
    --preset medium --crf 22 --crf 28 \
    --score-backend cuda \
    --sample-clip-seconds 10 \
    --auto-hdr \
    --output smoke.jsonl
```

`--encoder` takes one value per run; a second `--encoder` replaces the
first. To cover several encoders, run the command once for each, or use
`vmaf-tune compare --encoders` (see
[codec adapters](vmaf-tune-codec-adapters.md#adapter-selection)).

## History

- **2026-05-08** ([ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md)): the
  HDR model slot was registered. ADR status: accepted for the encode
  side; HDR-VMAF scoring deferred because no HDR model ships in the tree.
  Checked against Netflix `master`, whose `model/` directory has no
  `vmaf_hdr_*.json`.

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the
  adapters the HDR flags are injected into.
- [`vmaf-tune-codec-hardware.md`](vmaf-tune-codec-hardware.md): the 10-bit
  hardware encoders.
- [`vmaf-tune-score-backend.md`](vmaf-tune-score-backend.md): the
  orthogonal `--score-backend` flag.
- [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md): sample-clip with
  `--two-pass`.
- [ADR-0300](../adr/0300-vmaf-tune-hdr-aware.md) and
  [ADR-0301](../adr/0301-vmaf-tune-sample-clip.md): the design decisions.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md):
  the audit that triggered this page.
