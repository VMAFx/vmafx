<!-- markdownlint-disable MD060 -->
# `vmaf-tune` codec adapters

`vmaf-tune corpus` and `vmaf-tune per-shot` drive an FFmpeg encode for
each grid cell via a *codec adapter* — a small Python module under
`tools/vmaf-tune/src/vmaftune/codec_adapters/` that translates the
adapter-agnostic CRF / preset / GOP knobs into the right argv shape
for the wrapped encoder. This page is the matrix of adapters shipped
on the fork plus the per-adapter caveats. The base tool is documented
in [`vmaf-tune.md`](vmaf-tune.md); the FFmpeg-filter integration is
in [`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md).

## Adapter selection

```shell
vmaf-tune corpus --encoder <adapter-name> ...
```

`--encoder` accepts any of the adapter slugs in the table below; pass
`--encoder` multiple times to fan a single source out across several
encoders in one corpus row.

`vmaf-tune compare` also accepts runtime-variant display tokens of the
form `ADAPTER@VARIANT`. The part before `@` is still the adapter slug
from this table; the variant suffix is a report label and runtime
binding key. Use `--encoder-ffmpeg-bin ADAPTER@VARIANT=/path/to/ffmpeg`
when two rows use the same FFmpeg encoder name through different FFmpeg
builds, for example mainline `libsvtav1` vs `libsvtav1@svt-av1-hdr`.
Corpus JSONL generation does not use this suffix yet; it still records
one real adapter name per row.

## Adapter matrix

| `--encoder` value   | Codec | Backend | ADR                                                        | Status           | Two-pass | Notes                                                     |
|---------------------|-------|---------|------------------------------------------------------------|------------------|----------|-----------------------------------------------------------|
| `libx264`           | H.264 | CPU     | [ADR-0237](../adr/0237-quality-aware-encode-automation.md) | Phase A baseline | yes      | Reference adapter; uses FFmpeg `-pass` / `-passlogfile` (omits `-crf` in multi-pass). |
| `libx265`           | HEVC  | CPU     | [ADR-0288](../adr/0288-vmaf-tune-codec-adapter-x265.md)    | Accepted         | yes      | x265-style preset names; same CRF range as libx264.       |
| `libaom-av1`        | AV1   | CPU     | [ADR-0279](../adr/0279-vmaf-tune-codec-adapter-libaom.md)  | Accepted         | no       | `--cpu-used` mapped from preset; long-encode warning.     |
| `libsvtav1`         | AV1   | CPU     | [ADR-0294](../adr/0294-vmaf-tune-codec-adapter-svtav1.md)  | Accepted         | no       | SVT-AV1 preset 0–13 mapped from `--preset` slug. Also carries the `libsvtav1@svt-av1-hdr` runtime variant (ADR-0644); its `-svtav1-params` knobs are tabulated in [vmaf-tune.md](vmaf-tune.md#svt-av1-hdr-tuning-knobs). |
| `libvpx-vp9`        | VP9   | CPU     | —                                                          | Accepted         | yes      | `-deadline good`, `-cpu-used`, `-crf`, `-b:v 0`.          |
| `libvvenc`          | VVC   | CPU     | [ADR-0285](../adr/0285-vmaf-tune-vvenc-nnvc.md)            | Accepted         | no       | VVenC (Fraunhofer); also covers vvenc-NNVC variant.       |
| `h264_nvenc`        | H.264 | NVENC   | [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md)        | Accepted         | no       | Requires Maxwell+ NVIDIA GPU + ffmpeg `--enable-nvenc`.   |
| `hevc_nvenc`        | HEVC  | NVENC   | [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md)        | Accepted         | no       | Same gating as h264_nvenc.                                |
| `av1_nvenc`         | AV1   | NVENC   | [ADR-0290](../adr/0290-vmaf-tune-nvenc-adapters.md)        | Accepted         | no       | Ada Lovelace+ silicon only (RTX 40-series and newer).     |
| `h264_qsv`          | H.264 | QSV     | [ADR-0281](../adr/0281-vmaf-tune-qsv-adapters.md)          | Accepted         | no       | Intel Quick Sync; Intel iGPU / Arc / Xeon-W.              |
| `hevc_qsv`          | HEVC  | QSV     | [ADR-0281](../adr/0281-vmaf-tune-qsv-adapters.md)          | Accepted         | no       | Same gating as h264_qsv.                                  |
| `av1_qsv`           | AV1   | QSV     | [ADR-0281](../adr/0281-vmaf-tune-qsv-adapters.md)          | Accepted         | no       | Arc A-series / Battlemage / Lunar Lake only.              |
| `h264_amf`          | H.264 | AMF     | [ADR-0282](../adr/0282-vmaf-tune-amf-adapters.md)          | Accepted         | no       | AMD Radeon (Polaris+) / Ryzen iGPU.                       |
| `hevc_amf`          | HEVC  | AMF     | [ADR-0282](../adr/0282-vmaf-tune-amf-adapters.md)          | Accepted         | no       | Same gating as h264_amf.                                  |
| `av1_amf`           | AV1   | AMF     | [ADR-0282](../adr/0282-vmaf-tune-amf-adapters.md)          | Accepted         | no       | RDNA 3 (RX 7000-series) and newer.                        |
| `h264_videotoolbox` | H.264 | VTB     | [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) | Accepted         | no       | macOS only; uses Apple Silicon Media Engine when present. |
| `hevc_videotoolbox` | HEVC  | VTB     | [ADR-0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) | Accepted         | no       | macOS only; same gating.                                  |

## Multi-codec corpus

Pass `--encoder` more than once to fan one source out across multiple
encoders in a single corpus row (per
[ADR-0297](../adr/0297-vmaf-tune-encode-multi-codec.md)):

```shell
vmaf-tune corpus \
    --source ref.yuv \
    --width 1920 --height 1080 --pix-fmt yuv420p --framerate 24 \
    --encoder libx264 --encoder libx265 --encoder hevc_nvenc \
    --preset medium --crf 22 --crf 28 --crf 34 \
    --out corpus.jsonl
```

Each `(encoder, preset, crf)` triple becomes one row in the JSONL
output; the `encoder` field disambiguates rows that share `preset` /
`crf`.

## Preset mapping

The adapter-agnostic `--preset` slug (`ultrafast` … `placebo` style for
software adapters; `p1` … `p7` for NVENC) is mapped per-adapter:

| Adapter family   | Slug source                                 | Notes                                              |
|------------------|---------------------------------------------|----------------------------------------------------|
| `libx264`        | x264 native preset names                    | Pass-through.                                      |
| `libx265`        | x265 native preset names                    | Pass-through.                                      |
| `libaom-av1`     | `--cpu-used 0…9` mapped from slug           | `placebo`→0, `medium`→4, `ultrafast`→9.            |
| `libsvtav1`      | `--preset 0…13` mapped from slug            | Inverse — `placebo`→0, `medium`→7, `veryfast`→13. |
| `libvpx-vp9`     | `--cpu-used 0…5` mapped from slug           | `placebo`/`slowest`→0, `medium`→3, `ultrafast`→5.  |
| `libvvenc`       | VVenC native preset names                   | Pass-through (`slower`, `medium`, `faster`).       |
| `*_nvenc`        | NVENC `p1`…`p7` accepted directly           | `--preset p4` etc.                                 |
| `*_qsv`          | QSV `veryslow`…`veryfast` accepted directly | Pass-through.                                      |
| `*_amf`          | AMF `quality` / `balanced` / `speed`        | Slug → quality knob.                               |
| `*_videotoolbox` | Quality 0.0–1.0                             | Slug → quality knob.                               |

The exact mapping table lives in each adapter's `preset_to_argv`
function; run `python -m vmaftune.codec_adapters.<adapter> --print-mapping`
to dump it (the helper is documented in the adapter's own module
docstring).

## Selecting adapters by host capability

The corpus subparser does not auto-skip unavailable adapters — passing
`--encoder h264_nvenc` on a host without a working NVENC ffmpeg raises
a clear error. To probe what is available before running a fan-out
sweep:

```shell
ffmpeg -hide_banner -encoders | grep -E "(nvenc|qsv|amf|videotoolbox|svtav1|aom-av1|x264|x265|vvenc)"
```

## See also

- [`vmaf-tune.md`](vmaf-tune.md) — the base tool, corpus + recommend
  flow.
- [`vmaf-tune-ffmpeg.md`](vmaf-tune-ffmpeg.md) — the
  `vf_libvmaf_tune` filter that wraps the same logic into a
  one-shot FFmpeg invocation.
- [ADR-0237](../adr/0237-quality-aware-encode-automation.md) — the
  Phase-A reference adapter shape and the six-phase roadmap.
- [Research-0086](../research/0086-usage-doc-coverage-audit-2026-05-08.md)
  — audit that triggered this page.
