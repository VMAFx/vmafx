# 0546 — Codec-adapter `two_pass_args` real implementations

Research digest for ADR-0546. Maps the four observed vendor models for
two-pass encoding to a single adapter contract.

## Software encoders with a generic ffmpeg 2-pass surface

`libx264`, `libvpx-vp9`, `libaom-av1`, `libvvenc` all accept FFmpeg's
generic `-pass N -passlogfile <prefix>` pair. FFmpeg writes the
stream-specific stats sidecar at `<prefix>-0.log` and an optional
mbtree companion at `<prefix>-0.log.mbtree` (libx264-specific). Pass 1
discards the bitstream via `-f null -`; pass 2 reads the sidecar and
writes the real output.

Verified against FFmpeg n8.1.1:

```text
$ ffmpeg -y -f lavfi -i testsrc=size=128x128:duration=0.5:rate=24 \
    -pix_fmt yuv420p -c:v libaom-av1 -crf 35 \
    -pass 1 -passlogfile /tmp/aom -f null -
$ ls -l /tmp/aom-0.log
-rw-r--r-- … 1 kilian kilian 1.2K … /tmp/aom-0.log
```

`libx265` is the outlier — it routes pass control through its own
`-x265-params pass=N:stats=<path>` payload (ADR-0333). The harness's
driver handles both shapes via the adapter's `two_pass_args` method
without branching on codec identity.

## SVT-AV1 CRF-mode prohibition

SVT-AV1 v4.1.0 enforces "CRF does not support multi-pass" at runtime.
Verified by running pass 2 against an existing stats sidecar:

```text
Svt[error]: CRF does not support multi-pass. Use single pass.
[libsvtav1 @ …] Error setting encoder parameters: bad parameter (0x80001005)
```

The harness pins libsvtav1 to CRF mode (the canonical quality axis),
so `supports_two_pass = False`. The adapter still returns
`-pass N -passlogfile <prefix>` from `two_pass_args` for callers that
explicitly switch into VBR via `extra_params`. FFmpeg's `libsvtav1`
wrapper *does* honour the generic `-pass` flag in VBR mode; the
codec-private `-svtav1-params passes=...` keys mentioned in some
SvtAv1 documentation are standalone-app-only and are rejected by the
FFmpeg wrapper (`Error parsing option passes: 2`).

## Hardware encoders: single-invocation in-encoder analysis

NVENC, QSV, and AMF all expose 2-pass-equivalent quality through a
single ffmpeg invocation with vendor-specific flags:

| Vendor | Flag set | FFmpeg AVOption source |
| --- | --- | --- |
| NVIDIA NVENC | `-multipass fullres` | `libavcodec/nvenc.c` (`fullres` = full-resolution analysis; `qres` = quarter-resolution variant) |
| Intel QSV | `-extbrc 1 -look_ahead_depth 40` | `libavcodec/qsvenc.c` (extended BRC + 40-frame look-ahead, the libmfx-sample-app "quality" preset default) |
| AMD AMF | `-preanalysis true` | `libavcodec/amfenc_h264.c` / `amfenc_hevc.c` / `amfenc_av1.c` |

These flags reside *inside* a single encoder invocation; there is no
on-disk stats sidecar and no separate first-pass run. The harness's
two-invocation driver would either re-encode from scratch twice or
fail outright (NVENC's `-multipass` requires VBR + target bitrate;
CRF mode rejects it). The adapter therefore declares
`supports_two_pass = False` so the driver falls back to single-pass.
Callers wanting the quality boost compose the pass-1 argv into
`extra_params`:

```python
boost = get_adapter("h264_nvenc").two_pass_args(1, Path("/tmp/_unused"))
# ('-multipass', 'fullres')
```

## VideoToolbox API limitation

Apple's `VTCompressionSession` C API (the one FFmpeg's
`h264_videotoolbox` / `hevc_videotoolbox` / `av1_videotoolbox` /
`prores_videotoolbox` encoders wrap) has no multi-pass interface. The
encoder runs single-pass only and exposes quality through `-q:v`
(constant quality), `-bitrate` (target bitrate), or — for ProRes —
the `-profile:v` tier selection.

The adapter raises the new `VideoToolboxTwoPassUnsupportedError`
(a `NotImplementedError` subclass so existing callers that catch the
broader exception keep working) with a message that names the encoder
and points users at the software fallback set
(`libx264` / `libx265` / `libsvtav1` / `libaom-av1` / `libvvenc`) —
all of which ship in the same FFmpeg build.

## Verification table

| Adapter | `supports_two_pass` | Strategy | Smoke result |
| --- | --- | --- | --- |
| `libx264` | True | `-pass N -passlogfile <prefix>` | argv emitted; full driver requires VBR (pre-existing CRF-mode behaviour) |
| `libx265` | True | `-x265-params pass=N:stats=<path>` | covered by Phase F (ADR-0333) regression suite |
| `libvpx-vp9` | True | `-pass N -passlogfile <prefix>` | end-to-end driver OK against synthetic 128×128 source |
| `libsvtav1` | False | Adapter returns VBR-mode argv; driver short-circuits to single-pass | runtime-confirmed prohibition (`CRF does not support multi-pass`) |
| `libaom-av1` | True | `-pass N -passlogfile <prefix>` | end-to-end driver OK against synthetic 128×128 source |
| `libvvenc` | True | `-pass N -passlogfile <prefix>` | not installed on local ffmpeg; argv verified, runtime path is documented |
| `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` | False | `-multipass fullres` returned for pass 1 | argv emitted; full hardware encode requires VBR + bitrate target (vendor constraint) |
| `h264_qsv` / `hevc_qsv` / `av1_qsv` | False | `-extbrc 1 -look_ahead_depth 40` returned for pass 1 | argv emitted; runtime requires QSV hardware |
| `h264_amf` / `hevc_amf` / `av1_amf` | False | `-preanalysis true` returned for pass 1 | argv emitted; runtime requires AMD GPU + AMF library |
| `h264_videotoolbox` / `hevc_videotoolbox` / `av1_videotoolbox` / `prores_videotoolbox` | False | raises `VideoToolboxTwoPassUnsupportedError` | error message verified by pytest |

## Out of scope

- Refactoring the `run_two_pass_encode` driver to support the
  single-invocation hardware analogue. Today the contract is "if
  `supports_two_pass` is False, run single-pass". Composing the
  hardware-quality-boost flags is a caller responsibility (documented
  in `docs/usage/vmaf-tune.md`).
- Pre-existing CRF-mode behaviour of `libx264` 2-pass (also requires
  VBR/bitrate target; not introduced by this PR).
- Adding `hardware_quality_boost` as a separate method on the
  protocol. Today `two_pass_args(1, _)` doubles as that surface.
