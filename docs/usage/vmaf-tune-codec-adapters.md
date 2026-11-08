<!-- markdownlint-disable MD060 -->
# `vmaf-tune` codec adapters

Pick the encoder for a `vmaf-tune` run with `--encoder <adapter>`. Each
adapter is a small Python module under
`tools/vmaf-tune/src/vmaftune/codec_adapters/` that turns the shared
CRF / preset / GOP knobs into the argv of one FFmpeg encoder, so the
search loops never branch on codec identity. This page lists the 19
registered adapters, how presets map onto each encoder, and the contract a
new adapter implements.

The base tool is [`vmaf-tune.md`](vmaf-tune.md). The FFmpeg-filter
integration is [`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md).

## Quick start

List the adapters your checkout knows, then name one on the command line:

```shell
python3 -c "import vmaftune.codec_adapters as c; print(c.known_codecs())"
vmaf-tune corpus --encoder libsvtav1 --preset medium --crf 35 \
    --source ref.yuv --width 1920 --height 1080 --output corpus.jsonl
```

The `--encoder` choices of `corpus`, `recommend`, `tune-per-shot`,
`recommend-saliency`, `ladder`, `fast` and `prefilter` are generated from
the registry, so every adapter below is accepted.

## Adapter matrix

The detail pages hold the per-encoder behaviour:

- [Software encoders](vmaf-tune-codec-software.md): `libx264`, `libx265`,
  `libvpx-vp9`, `libvvenc`.
- [AV1 encoders](vmaf-tune-codec-av1.md): `libaom-av1`, `libsvtav1` and
  the `libsvtav1@svt-av1-hdr` runtime variant.
- [Hardware encoders](vmaf-tune-codec-hardware.md): NVENC, QSV, AMF and
  Apple VideoToolbox.

| `--encoder` value | Codec | Backend | Quality knob (range, default) | Two-pass | ADR |
|---|---|---|---|---|---|
| `libx264` | H.264 | CPU | `crf` 0..51, 23 | yes | [0237](../adr/0237-quality-aware-encode-automation.md) |
| `libx265` | HEVC | CPU | `crf` 15..40, 28 | yes | [0288](../adr/0288-vmaf-tune-codec-adapter-x265.md) |
| `libaom-av1` | AV1 | CPU | `crf` 0..63, 35 | yes | [0279](../adr/0279-vmaf-tune-codec-adapter-libaom.md) |
| `libsvtav1` | AV1 | CPU | `crf` 20..50, 35 | no (CRF mode) | [0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md) |
| `libvpx-vp9` | VP9 | CPU | `crf` 0..63, 32 | yes | none |
| `libvvenc` | VVC | CPU | `qp` 17..50, 32 | yes | [0285](../adr/0285-vmaf-tune-vvenc-nnvc.md) |
| `h264_nvenc` | H.264 | NVENC | `cq` 0..51, 23 | no | [0290](../adr/0290-vmaf-tune-nvenc-adapters.md) |
| `hevc_nvenc` | HEVC | NVENC | `cq` 0..51, 23 | no | [0290](../adr/0290-vmaf-tune-nvenc-adapters.md) |
| `av1_nvenc` | AV1 | NVENC | `cq` 0..51, 23 | no | [0290](../adr/0290-vmaf-tune-nvenc-adapters.md) |
| `h264_qsv` | H.264 | QSV | `global_quality` 1..51, 23 | no | [0281](../adr/0281-vmaf-tune-qsv-adapters.md) |
| `hevc_qsv` | HEVC | QSV | `global_quality` 1..51, 23 | no | [0281](../adr/0281-vmaf-tune-qsv-adapters.md) |
| `av1_qsv` | AV1 | QSV | `global_quality` 1..51, 23 | no | [0281](../adr/0281-vmaf-tune-qsv-adapters.md) |
| `h264_amf` | H.264 | AMF | `qp` 15..40, 23 | no | [0282](../adr/0282-vmaf-tune-amf-adapters.md) |
| `hevc_amf` | HEVC | AMF | `qp` 15..40, 23 | no | [0282](../adr/0282-vmaf-tune-amf-adapters.md) |
| `av1_amf` | AV1 | AMF | `qp` 15..40, 23 | no | [0282](../adr/0282-vmaf-tune-amf-adapters.md) |
| `h264_videotoolbox` | H.264 | VideoToolbox | `q:v` 0..100, 50 | no | [0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) |
| `hevc_videotoolbox` | HEVC | VideoToolbox | `q:v` 0..100, 50 | no | [0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) |
| `prores_videotoolbox` | ProRes | VideoToolbox | `profile:v` tier 0..5, 3 | no | [0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) |
| `av1_videotoolbox` | AV1 | VideoToolbox | `q:v` 0..100, 50 | no | [0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md) |

Reading the table:

- The range is what the adapter accepts. A value outside it raises
  `ValueError` before FFmpeg starts.
- `av1_videotoolbox` is a placeholder: it is registered, but `validate()`
  raises until the local FFmpeg exposes an `av1_videotoolbox` encoder.
- NVENC validates against the hardware window 0..51. The 15..40
  window, shared with `libx264`, is only its informative grid default.
  AMF enforces 15..40.
- Two-pass "no" means `--two-pass` falls back to single-pass for that
  encoder. [Multi-pass encoding](vmaf-tune-multipass.md) explains why
  and what the hardware encoders offer instead.
- VideoToolbox quality is higher-is-better (`invert_quality` is false);
  every other adapter is lower-is-better.

## Adapter selection

`corpus --encoder` takes exactly one adapter per run. To cover several
encoders, run `corpus` once per encoder or use `compare`:

```shell
vmaf-tune compare --src ref.yuv --width 1920 --height 1080 \
    --target-vmafs 92,95 --encoders libx265,libsvtav1,hevc_nvenc
```

`compare --encoders` takes a comma-separated list. Its default is the CPU
set `libx265,libsvtav1` (ADR-0641), and a hardware encoder the host cannot
run is skipped with a recorded reason instead of failing the run.
[ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md) made the encode
driver codec-agnostic; the fan-out across encoders lives in `compare`.

`compare` also accepts runtime-variant tokens of the form
`ADAPTER@VARIANT`:

- The part before `@` is the adapter slug from the table above.
- The variant is a report label and provenance key. It never changes the
  argv.
- `--encoder-ffmpeg-bin ADAPTER@VARIANT=/path/to/ffmpeg` binds the token
  to one FFmpeg build; unbound tokens use `--ffmpeg-bin`.

!!! note "Variants are a `compare` feature"
    `corpus` does not read the `@VARIANT` suffix. It records one real
    adapter name per row.

## Preset mapping

`--preset` takes the x264-style names `ultrafast` ... `placebo`. Each
adapter accepts the subset it lists and maps it onto the encoder's native
scale:

| Adapter | Accepted names | Native value |
|---|---|---|
| `libx264` | `ultrafast` ... `veryslow` (9 names) | same name |
| `libx265` | `ultrafast` ... `veryslow`, `placebo` (10) | same name |
| `libaom-av1` | all 10 | `-cpu-used` 0..9, `placebo`=0, `medium`=4, `ultrafast`=9 |
| `libsvtav1` | `placebo` ... `veryfast` (8) | `-preset` 0, 1, 3, 5, 7, 9, 11, 13; `medium`=7 |
| `libvpx-vp9` | all 10 | `-cpu-used` 0..5, `medium`=3, `ultrafast`=5 |
| `libvvenc` | all 10 | five native presets, `medium` stays `medium` |
| `*_nvenc` | all 10 | `-preset p1`..`p7`, `medium`=`p4` |
| `*_qsv` | `veryslow` ... `veryfast` (7) | same name |
| `*_amf` | all 10 | `-quality` `speed` / `balanced` / `quality` |
| `*_videotoolbox` | `ultrafast` ... `veryslow` (9) | `-realtime 1` or `0` |

The per-encoder pages give the full maps. The authoritative maps are the
adapter modules themselves, in each module's preset table and
`ffmpeg_codec_args()`.

## Selecting adapters by host capability

`corpus` does not skip an unavailable adapter. With `--encoder
h264_nvenc` on a host whose FFmpeg has no NVENC, the encode exits
non-zero and the cell's scoring is skipped. Probe the build before a
fan-out sweep:

```shell
ffmpeg -hide_banner -encoders | grep -E "(nvenc|qsv|amf|videotoolbox|svtav1|aom-av1|x264|x265|vvenc)"
```

## Adapter contract

The encode driver (`tools/vmaf-tune/src/vmaftune/encode.py`) is
codec-agnostic since [ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md).
It calls `vmaftune.codec_adapters.get_adapter(req.encoder)` and asks the
adapter for its argv slice. Adding a codec is one file under
`codec_adapters/` plus a registry entry in `codec_adapters/__init__.py`.
The search loops, the corpus row schema and the FFmpeg invocation do not
change.

An adapter is a frozen dataclass with these members:

| Member | Type | Purpose |
|---|---|---|
| `name`, `encoder` | `str` | Adapter slug and the FFmpeg `-c:v` value. |
| `quality_knob` | `str` | Knob name: `crf`, `cq`, `qp`, `global_quality`, `q:v`, `profile:v`. |
| `quality_range` | `tuple[int, int]` | Inclusive `(min, max)` the adapter accepts. |
| `quality_default` | `int` | Value used when `--crf` is omitted. |
| `invert_quality` | `bool` | True when a higher value means lower quality. |
| `presets` | `tuple[str, ...]` | Accepted preset names. |
| `adapter_version` | `str` | Bumped when the argv shape, presets or range change (cache key input). |
| `probe_preset`, `probe_quality` | `str`, `int` | Fast probe encode used as a complexity barometer. |
| `supports_qpfile`, `supports_encoder_stats`, `supports_two_pass` | `bool` | Capability flags read by saliency, stats capture and `--two-pass`. |
| `two_pass_abr_at_pass1_bitrate` | `bool` (optional, `libx265` only) | A `--two-pass` cell at a CRF is pass 1 at the CRF, then ABR at pass 1's bitrate; see [multi-pass](vmaf-tune-multipass.md#libx265-at-a-crf). Read with `getattr(..., False)`, so other adapters need not declare it. |
| `validate(preset, quality)` | method | Raises `ValueError` on unsupported input. |
| `ffmpeg_codec_args(preset, quality)` | method | The `-c:v ...` argv slice. |
| `extra_params()` | method | Extra argv, for example `("-row-mt", "1")` for `libvpx-vp9`. |
| `gop_args()`, `force_keyframes_args()` | methods | GOP and forced-keyframe argv. |
| `probe_args()` | method | Argv of the probe encode. |
| `two_pass_args(pass, stats_path)` | method | Argv of pass N; see [multi-pass](vmaf-tune-multipass.md). |

The dispatcher builds the command in this order:

```text
[ffmpeg, -y, -hide_banner, -loglevel info,
 <input args: rawvideo geometry, -ss/-t window, -i <src>>,
 *adapter.ffmpeg_codec_args(preset, quality),
 *adapter.extra_params(),
 *two-pass args (pass 1 or 2 only),
 *req.extra_params,
 <output>]
```

Two fallbacks keep partial adapters drivable. An encoder missing from the
registry, or an adapter without `ffmpeg_codec_args`, falls back to the
legacy `-c:v <encoder> -preset <p> -crf <q>` shape. `parse_versions()`
picks a per-codec version probe and returns `"unknown"` instead of
raising when nothing matches.

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool and subcommand overview.
- [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md): two-pass support
  per adapter.
- [`vmaf-tune-hdr-and-sampling.md`](vmaf-tune-hdr-and-sampling.md): the
  per-encoder HDR flags.
- [`vmaf-tune-saliency-aware.md`](vmaf-tune-saliency-aware.md): which
  adapters take ROI maps.
- [`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md): the `vmafx_tune`
  filter.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md): the
  reference adapter shape and the phase roadmap.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md):
  the audit that triggered this page.
