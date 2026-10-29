<!-- markdownlint-disable MD060 -->
# `vmaf-tune` hardware encoders: NVENC, QSV, AMF and VideoToolbox

Use a hardware adapter when you need a large corpus quickly or when the
production pipeline is GPU-encoded. The adapters share the software
adapters' contract, so the search loop is identical; only the FFmpeg
argv differs. This page covers the four hardware families. The registry
of all adapters is
[`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md).

## Quick start

```shell
vmaf-tune corpus \
    --encoder h264_nvenc \
    --source ref.yuv --width 1920 --height 1080 --pix-fmt yuv420p \
    --framerate 24 --duration 10 \
    --preset slow --preset medium --preset fast \
    --crf 23 --crf 28 --crf 34 \
    --output corpus_nvenc.jsonl
```

Swap `--encoder` for any adapter below. Requirements are the same for
every family: an FFmpeg build with the vendor encoder compiled in, the
matching GPU, and its driver or runtime.

## The four families

| Family | Adapters | Quality knob (accepted range) | Default |
|---|---|---|---|
| NVIDIA NVENC | `h264_nvenc`, `hevc_nvenc`, `av1_nvenc` | `-cq` (0..51) | 23 |
| Intel QSV | `h264_qsv`, `hevc_qsv`, `av1_qsv` | `-global_quality` (1..51) | 23 |
| AMD AMF | `h264_amf`, `hevc_amf`, `av1_amf` | `-qp_i` / `-qp_p` (15..40) | 23 |
| Apple VideoToolbox | `h264_videotoolbox`, `hevc_videotoolbox`, `prores_videotoolbox`, `av1_videotoolbox` | `-q:v` (0..100), `-profile:v` tier (0..5) | 50, tier 3 |

Common behaviour:

- The adapters do not check the GPU generation. A missing encoder or an
  unsupported GPU makes the encode exit non-zero, and the harness
  records the failure and skips scoring, so a partial corpus from a
  mixed fleet is still well-formed.
- Every hardware encoder (NVENC, QSV, AMF and VideoToolbox) is probed
  with a one-frame dummy encode before it is used. `compare` skips a
  failing one with a recorded reason; `corpus`, live `recommend` and
  `ladder` stop before the first encode with exit status 2 and the
  probe's reason.
- None of the hardware adapters supports the two-invocation `--two-pass`
  driver. NVENC, QSV and AMF offer an in-encoder look-ahead instead; see
  [multi-pass encoding](vmaf-tune-multipass.md).
- Hardware encoders write no parseable first-pass stats file.

## NVIDIA NVENC

| Adapter | FFmpeg encoder | Hardware |
|---|---|---|
| `h264_nvenc` | `h264_nvenc` | NVIDIA Kepler or newer |
| `hevc_nvenc` | `hevc_nvenc` | NVIDIA Maxwell 2nd generation or newer (GTX 960 and up) |
| `av1_nvenc` | `av1_nvenc` | NVIDIA Ada Lovelace or newer (RTX 40 series, L40, L4) |

The FFmpeg build needs `--enable-nvenc`. The quality knob is `-cq`, the
closest analogue of libx264 CRF. The hardware accepts 0..51; the
informative grid window is 15..40, the same as `libx264`.

NVENC has seven preset levels, `p1` (fastest) to `p7` (slowest). The CLI
takes the shared mnemonic names and maps them:

| Mnemonic | NVENC preset |
|---|---|
| `ultrafast`, `superfast`, `veryfast` | `p1` |
| `faster` | `p2` |
| `fast` | `p3` |
| `medium` (default) | `p4` |
| `slow` | `p5` |
| `slower` | `p6` |
| `slowest`, `placebo` | `p7` |

Forced keyframes (`--force-keyframes`) add `-forced-idr 1`, because NVENC
honours `-force_key_frames` only with it.

## Intel QSV

| Adapter | FFmpeg encoder | Hardware |
|---|---|---|
| `h264_qsv` | `h264_qsv` | Intel iGPU 7th generation (Kaby Lake) or newer, or Arc / Battlemage |
| `hevc_qsv` | `hevc_qsv` | Intel iGPU 7th generation or newer (10-bit needs 11th generation), or Arc / Battlemage |
| `av1_qsv` | `av1_qsv` | Intel iGPU 12th generation or newer, or Arc / Battlemage |

QSV uses ICQ rate control through `-global_quality` (1..51). Its seven
presets are `veryslow`, `slower`, `slow`, `medium`, `fast`, `faster` and
`veryfast`, passed through unchanged. QSV has no `ultrafast` or
`superfast`; those names are rejected.

`vmaf-tune` validates the `(preset, global_quality)` pair before spawning
FFmpeg. Every QSV encode (`corpus`, `ladder`, `compare`'s bisect, `fast`,
`tune-per-shot`) carries a VA-API / QSV device chain before the input and
the upload filter at the end of its `-vf` chain, from the same adapter
code the availability probe uses:

```shell
ffmpeg \
    -init_hw_device vaapi=va:/dev/dri/renderD129 \
    -init_hw_device qsv=qsv_dev@va \
    -filter_hw_device qsv_dev \
    -i src.mkv \
    -vf format=nv12,hwupload=extra_hw_frames=64 \
    -c:v h264_qsv -preset medium -global_quality 23 -an out.mkv
```

Without the chain every QSV encode fails with `-22 Invalid argument`. The
filter device is the QSV device (`qsv_dev`): with the VA-API device there,
as ADR-0601 first recorded it, `hwupload` produces `vaapi` frames and the
filter graph fails before the encoder opens. A ladder rung's
`scale=W:H` and the upload share one `-vf` chain
(`scale=W:H,format=nv12,hwupload=extra_hw_frames=64`), because FFmpeg
keeps only the last `-vf`.

The upload format follows the requested output depth: `nv12` for 8-bit,
`p010le` for `p010le` / `yuv420p10le` output or `-profile:v main10`.
Output `-pix_fmt` in extra parameters overrides the source pixel format;
HDR arguments select P010 automatically. For example, HEVC Main 10 uses
`-vf format=p010le,hwupload=extra_hw_frames=64 -c:v hevc_qsv -profile:v main10`.

!!! warning "QSV availability depends on the driver"
    The argv and the probe wiring are covered by tests with fakes. In an
    earlier Arc A380 measurement (Linux xe driver, iHD 26.3.5, oneVPL 2.17),
    the chain got past device and filter initialisation, but every QSV
    encoder then failed inside the runtime
    with `Invalid FrameType:0`, with or without `vmaf-tune`'s chain. A
    `LIBVA_DRIVER_NAME` that names another vendor's driver also breaks the
    device chain ("Failed to get device id from the driver").

The VA-API render node defaults to `auto`: `vmaf-tune` walks
`/dev/dri/by-path` and `/sys/class/drm/renderD*/device/vendor`, picks
the first Intel vendor node (`0x8086`), and falls back to
`/dev/dri/renderD128` only when none is found. Pin a node with
`vmaf-tune compare --vaapi-device /dev/dri/renderD129` or with the
`VMAFTUNE_VAAPI_DEVICE` environment variable (the flag wins). The
environment variable applies to every subcommand that encodes; the flag
exists on `compare` and holds for its probe and every encode of the run.

## AMD AMF

| Adapter | Hardware | FFmpeg flag |
|---|---|---|
| `h264_amf` | any AMD GPU with AMF | `-c:v h264_amf` |
| `hevc_amf` | any AMD GPU with AMF | `-c:v hevc_amf` |
| `av1_amf` | RDNA 3 or newer (RX 7000 series) | `-c:v av1_amf` |

Requirements: FFmpeg built with `--enable-amf`, and the AMF runtime
(Adrenalin on Windows; the Mesa AMF stack or the AMD Pro driver on
Linux). `ensure_amf_available()` raises a `RuntimeError` when the
encoder is missing, but `corpus` does not call it. Rate control is
constant-QP: `-rc cqp` with matched `-qp_i` and
`-qp_p`, range 15..40 (the hardware allows 0..51).

AMF exposes three `-quality` rungs where the other families expose seven
or more presets, so the adapter compresses the names:

| Preset names | AMF `-quality` |
|---|---|
| `placebo`, `slowest`, `slower`, `slow` | `quality` |
| `medium` (default) | `balanced` |
| `fast`, `faster`, `veryfast`, `superfast`, `ultrafast` | `speed` |

Callers that need finer control pin `-qp_i` / `-qp_p` instead of the
preset.

```shell
vmaf-tune corpus \
    --encoder h264_amf \
    --source ref.yuv --width 1920 --height 1080 \
    --preset slow --preset medium --preset fast \
    --crf 23 --crf 28 --crf 34 \
    --output corpus_amf.jsonl
```

The FFmpeg invocation for one cell:

```shell
ffmpeg -i ref.yuv -c:v h264_amf \
       -quality balanced -rc cqp -qp_i 23 -qp_p 23 \
       -an out.mkv
```

Each of `-quality`, `-rc`, `-qp_i` and `-qp_p` appears once. Until
2026-10-04 the AMF adapters returned the same block from
`extra_params()` as well, so every AMF command line carried it twice.

!!! note "Some APUs have no AMF encoder"
    The gfx1036 iGPU in AMD Raphael and Phoenix APUs (Ryzen 7000
    integrated graphics) is decode-only. `h264_amf`, `hevc_amf` and
    `av1_amf` fail there with `AMF_NOT_SUPPORTED` even with the AMF
    runtime installed. `compare` reports it as `hardware encoder not
    available: ... dummy encode failed` and carries on.

## Apple VideoToolbox

| Adapter | FFmpeg encoder | Quality knob | Hardware |
|---|---|---|---|
| `h264_videotoolbox` | `h264_videotoolbox` | `-q:v` 0..100, higher is better | Apple Silicon, or Intel Mac with T2 |
| `hevc_videotoolbox` | `hevc_videotoolbox` | `-q:v` 0..100, higher is better | Apple Silicon, or Intel Mac with T2 |
| `prores_videotoolbox` | `prores_videotoolbox` | `-profile:v` tier 0..5 | Apple Silicon M1 Pro / Max / Ultra or later |
| `av1_videotoolbox` | `av1_videotoolbox` | `-q:v` 0..100 (placeholder) | none shipped yet |

The adapters validate the `(preset, quality)` pair (`-q:v` for H.264
and HEVC, the integer tier id for ProRes). The host is probed like every
hardware encoder's: on a machine without VideoToolbox, for example
Linux, the one-frame dummy encode fails and `compare` records the row as
unavailable, while `corpus` and `ladder` stop with exit status 2.

VideoToolbox has no multi-valued preset, only a boolean `-realtime`
flag, so the nine accepted names collapse onto it:

| Preset names | `-realtime` |
|---|---|
| `ultrafast`, `superfast`, `veryfast`, `faster`, `fast` | `1` (low latency) |
| `medium`, `slow`, `slower`, `veryslow` | `0` (offline, quality first) |

The mapping is lossy by design. `placebo` and `slowest` are not accepted
here. The FFmpeg invocations:

```shell
ffmpeg -i src.mkv -c:v h264_videotoolbox -realtime 0 -q:v 60 -an out.mkv
ffmpeg -i src.mkv -c:v hevc_videotoolbox -realtime 0 -q:v 60 -an out.mkv
ffmpeg -i src.mkv -c:v prores_videotoolbox -realtime 0 -profile:v hq -an out.mov
```

### AV1 placeholder

`av1_videotoolbox` is registered but inactive. `validate()` raises
`Av1VideoToolboxUnavailableError` until a probe
(`ffmpeg -h encoder=av1_videotoolbox`) shows that the local FFmpeg
exposes the encoder, at which point it activates with no code change
([ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md)). Until
then use `libaom-av1` or `libsvtav1`; see
[AV1 encoders](vmaf-tune-codec-av1.md).

### ProRes tiers

ProRes is a fixed-rate intermediate codec with no CRF or QP scalar. The
tier selects quality, and the bitrate follows from tier, resolution and
frame rate. `--crf` carries the integer tier id (FFmpeg's `profile:v`
value); the adapter writes the FFmpeg alias on the argv.

| `--crf` value | FFmpeg alias | Marketing name | Typical use |
|---|---|---|---|
| 0 | `proxy` | ProRes 422 Proxy | Offline editing, dailies |
| 1 | `lt` | ProRes 422 LT | Broadcast acquisition |
| 2 | `standard` | ProRes 422 | Mainline broadcast master |
| 3 | `hq` | ProRes 422 HQ | High-end broadcast or film master (default) |
| 4 | `4444` | ProRes 4444 | Graphics, alpha, colour grading |
| 5 | `xq` | ProRes 4444 XQ | High dynamic range or wide gamut master |

The tiers come from FFmpeg's `libavcodec/videotoolboxenc.c`
`prores_options` table in the supported FFmpeg `n9.0.2` baseline.
ProRes is intra-only, so `--keyint` and `--force-keyframes` are accepted
but have no rate-distortion effect; the harness still emits them so the
muxer's seek-table density stays predictable across codecs.

## Hardware or software

Hardware encoders are much faster than the software encoders at the cost
of quality. Typical figures, not a contract:

- NVENC is roughly 10x to 100x faster.
- `h264_nvenc` at `medium` typically loses 3 to 5 VMAF points against
  `libx264 medium` at the same bitrate, depending on content.

The Pareto frontier is genuinely different, which is why the harness
treats NVENC as separate codec entries instead of a flag on `libx264`.
Use hardware for a large corpus quickly or a GPU-encoded production
pipeline, and software when you need the best encode at a given bitrate.

## Troubleshooting

`Encoder h264_nvenc not found` (or a sibling encoder) means the FFmpeg
build lacks the encoder or the GPU lacks the generation. The harness
records `exit_status != 0` and skips scoring for that cell. List the
encoders your build has:

```shell
ffmpeg -hide_banner -encoders | grep -E "(nvenc|qsv|amf|videotoolbox)"
```

## See also

- [`vmaf-tune.md`](vmaf-tune.md): the base tool.
- [`vmaf-tune-codec-adapters.md`](vmaf-tune-codec-adapters.md): the
  adapter registry and contract.
- [`vmaf-tune-multipass.md`](vmaf-tune-multipass.md): the in-encoder
  look-ahead flags per hardware family.
- [`vmaf-tune-hdr-and-sampling.md`](vmaf-tune-hdr-and-sampling.md): the
  10-bit HDR flags per hardware encoder.
- ADRs: [0290](../adr/0290-vmaf-tune-nvenc-adapters.md) NVENC,
  [0281](../adr/0281-vmaf-tune-qsv-adapters.md) QSV,
  [0282](../adr/0282-vmaf-tune-amf-adapters.md) AMF,
  [0283](../adr/0283-vmaf-tune-videotoolbox-adapters.md) VideoToolbox,
  [0601](../adr/0601-vmaftune-qsv-amf-hw-init-and-probe-fix.md) and
  [0641](../adr/0641-dev-container-encoder-probe-hardening.md) device
  init and probes.

## QSV rate control probe

Intel Quick Sync Video (QSV) uses ICQ (`-global_quality N`) as its preferred
quality control. Before encoding, the Python tuner probes eight synthetic
`testsrc2` frames at 320×240 through the selected render node and encoder,
discarding the output with `-f null`. A successful probe keeps ICQ. A failed
probe selects CQP (`-q:v N`) with the same quality value; both modes accept
1–51. CQP and ICQ can produce different bitstreams and quality, so compare
results within the recorded mode.

`VMAFTUNE_QSV_RATE_CONTROL=auto` is the default. Set it to `icq` to force ICQ or
`cqp` to force CQP and skip the probe. An invalid value fails explicitly. For
example:

```bash
VMAFTUNE_QSV_RATE_CONTROL=cqp vmaf-tune corpus --help
```

Automatic results are cached for the process lifetime by resolved render node,
encoder and FFmpeg binary. Concurrent requests share one probe. A new process
probes again; a different FFmpeg binary has a separate cache entry.

Python corpus and compare JSON rows add `qsv_rate_control` (`"icq"` or
`"cqp-fallback"`), `qsv_rate_control_reason`, and the same entries inside
`provenance`. Forced CQP is also named `"cqp-fallback"`, with reason
`"VMAFTUNE_QSV_RATE_CONTROL=cqp"`. A probe failure reason includes its exit
status or execution error. Existing canonical corpus columns and schema version
remain unchanged; these are optional additive fields. QSV adapter version 4
invalidates prior adapter cache entries, and encode-cache keys distinguish
rate-control mode and render node.

The fallback addresses runtimes where ICQ fails even though default QSV encoding
works. On an Intel Arc A380 with `intel-media-driver` 26.3.5 the probe fails
with `Invalid FrameType:0`, and `vmaf-tune corpus` with `h264_qsv`, `hevc_qsv`
and `av1_qsv` completes through the CQP fallback, every row reporting
`qsv_rate_control=cqp-fallback` (measured 2026-10-28; see [the state
tracker](../state.md)).
