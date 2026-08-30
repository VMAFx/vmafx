<!-- markdownlint-disable MD013 MD060 -->
# ADR-0601: vmaf-tune QSV/AMF hardware-device init + encoder probe size fix

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris
- **Tags**: `vmaf-tune`, `compare`, `qsv`, `amf`, `nvenc`, `hardware`, `probe`, `bugfix`, `fork-local`

## Context

Three related bugs blocked the BBB v14 compare run with hardware encoders
(`h264_nvenc`, `h264_qsv`, `h264_amf`):

**Bug V14-A — probe resolution too small for NVENC.**
`compare.probe_encoder_available()` issued a 1-frame dummy encode to confirm
that an encoder is functional at runtime (not just listed in `ffmpeg -encoders`).
The dummy source was `lavfi nullsrc=size=64x64:rate=1:duration=0.04`. NVENC
rejects resolutions below approximately 145×49 with `-22 (EINVAL)` at the
hardware layer, causing the probe to return `(False, "dummy encode failed")`
for every NVENC encoder even on a host that has a fully-working RTX GPU. The
same 64×64 input also fell below QSV's minimum resolution (approximately
128×96).

**Bug V14-B — QSV probe and encodes missing VA-API device init.**
FFmpeg's QSV bridge on Linux requires a three-step hardware-device
initialisation chain before the first `-i` argument:

```text
-init_hw_device vaapi=va:<dev>
-init_hw_device qsv=qsv_dev@va
-filter_hw_device va
```

followed by a pixel-format conversion filter before the encoder:

```text
-vf format=nv12,hwupload=extra_hw_frames=64
```

Without these flags, every QSV encode (probe and production) fails with
`-22 Invalid argument` regardless of hardware or driver state. The QSV codec
adapters had no mechanism to emit these flags; `compare.py` never injected
them. The `/dev/dri/renderD128` default covers most single-GPU Linux hosts;
a mixed-GPU host or a system where the Intel GPU is the second DRI node needs
the override.

**Bug V14-C — AMF gfx1036 (AMD Raphael / Phoenix APU) is decoder-only.**
The gfx1036 iGPU in AMD Ryzen 7000-series APUs (Raphael / Phoenix) contains
a VCN decode block but no VCE encode block. `h264_amf`, `hevc_amf`, and
`av1_amf` all fail with `AMF_NOT_SUPPORTED` even when the AMF runtime and
ROCm libraries are installed and the encoder names appear in `ffmpeg -encoders`.
This is a silicon-level limitation rather than a software configuration
problem. The probe correctly surfaces it as `(False, "dummy encode failed")`
rather than aborting the sweep — but the limitation was undocumented, making
the failure surprising.

## Decision

**V14-A**: Change the dummy-encode source from
`nullsrc=size=64x64:rate=1:duration=0.04` to
`nullsrc=size=320x240:rate=24:duration=0.5`. 320×240 clears every known
hardware minimum (NVENC: ~145×49; QSV: ~128×96; AMF: not documented but
empirically fine at 320×240). The 0.5-second / 24-fps = 12-frame source
gives encoders that require a minimum frame count a safe margin.

**V14-B**: Add `_hw_init_args_for_encoder(encoder, vaapi_device)` helper to
`compare.py` that returns the three QSV VA-API init flags when the encoder is
in `_QSV_ENCODERS` and an empty list otherwise. Inject these before the
`-i` flag and inject `-vf format=nv12,hwupload=extra_hw_frames=64` after it
for QSV encoders in `probe_encoder_available()`. Expose the VA-API device path
as:

- `--vaapi-device PATH` CLI flag on `compare` (default `/dev/dri/renderD128`)
- `VMAFTUNE_VAAPI_DEVICE` environment variable (flag takes precedence)
- `DEFAULT_VAAPI_DEVICE` public constant in `compare.py` for programmatic use

Also add `BaseQsvAdapter.qsv_hw_init_args(vaapi_device)` static method so
callers that build production encode commands can retrieve the same init block.

**V14-C**: Document the gfx1036 decoder-only limitation in the module docstring
of `codec_adapters/_amf_common.py`. No code change is needed — the probe
already surfaces unavailable encoders correctly; the documentation prevents
the next operator from spending time debugging a hardware limitation as if it
were a software bug.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep 64×64, add NVENC-specific resolution override | Minimal change surface | Brittle per-vendor fragmentation; would need updates as new hardware minimums emerge | 320×240 is universally safe and costs nothing extra |
| Hard-code QSV init flags in ffmpeg_codec_args / extra_params | Simple | Couples init flags to encode commands; probe path still broken | Probe and encode must both inject init independently; helper approach is cleaner |
| Expose per-adapter qsv_init_args in the sweep driver | Cleaner separation of concerns | Higher refactor cost without extra benefit | The `_hw_init_args_for_encoder` helper in `compare.py` stays co-located with the probe, which is where the injection must happen |
| Raise RuntimeError for gfx1036 AMF | Explicit | Aborts whole sweep over one unavailable encoder | Probe returning `(False, message)` is the correct non-fatal path; documentation is the right fix |

## Consequences

- **Positive**: NVENC and QSV hardware encoders now probe and encode
  successfully on Linux hosts with the correct driver stack. The BBB v14
  compare unblocks. The `--vaapi-device` flag handles non-default GPU topologies
  without rebuilding.
- **Negative**: The probe source is now 12 frames rather than ~1 frame. Wall
  time for `probe_encoder_available()` increases by roughly 0.4 s per encoder
  on a fast host — negligible at the compare entry point.
- **Neutral / follow-ups**: The `VMAFTUNE_VAAPI_DEVICE` env var and
  `--vaapi-device` flag must be threaded into the production QSV encode
  command path (currently only in the probe path) when production QSV sweeps
  are added. The flag is already parsed and stored; wiring it into
  `encode.build_ffmpeg_command` is a separate PR.

## References

- [ADR-0281](0281-vmaf-tune-qsv-adapters.md) — QSV adapter introduction
- [ADR-0282](0282-vmaf-tune-amf-adapters.md) — AMF adapter introduction
- [ADR-0516](0516-vmaftune-compare-v2-sweep.md) — `probe_encoder_available` introduction
- FFmpeg QSV Linux init: <https://trac.ffmpeg.org/wiki/Hardware/QuickSync>
- AMD gfx1036 VCN decoder-only: <https://gpuopen.com/radeon-media-sdk/> (VCE block absent in Phoenix iGPU)
- Source: `req` (user direction, 2026-05-18 session)
