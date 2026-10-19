<!-- markdownlint-disable MD013 MD060 -->
# ADR-2167: `-qpfile` on libx264 applies its offsets through `quant_offsets`

- **Status**: Accepted (2026-10-07; shipped in #2385, decision Q-131)
- **Date**: 2026-10-07
- **Deciders**: maintainer (FFmpeg audit request, RC4 WP15); RC4 WP15
- **Tags**: rc4, ffmpeg, vmaf-tune, correctness

## Context

Patch `0007` of the FFmpeg series adds a `-qpfile <path>` option to the
libx264, libsvtav1 and libaom wrappers for the per-frame, per-macroblock QP
deltas `vmaf-tune`'s saliency step writes
([ADR-0312](0312-ffmpeg-patches-vmaf-tune-integration.md)). The libaom and
SVT-AV1 branches parse the file and map the deltas onto their region-of-interest
APIs. The libx264 branch handed the path to
`x264_param_parse(&params, "qpfile", path)`, on the belief that x264 reads
the file.

The FFmpeg audit of RC4 work package 15
ran it: libx264 165 has no `qpfile` parameter (the x264 command line reads
the file itself; the library does not), `x264_param_parse` returns a bad-name
error, and the encoder does not open (`libx264: failed to load qpfile=... (x264
ret=-1)`). Through `-x264-params qpfile=` (what `vmaf-tune`'s Go and Python
saliency code passed) FFmpeg only warns `Error parsing option` and encodes
without the ROI. Both libaom and SVT-AV1 accept the file and encode.

## Decision

1. **The libx264 branch parses the file with the shared parser and hands each
   frame's per-macroblock deltas to x264 as `quant_offsets`**, the channel
   FFmpeg's own region-of-interest side data uses (`setup_roi()`): record `n`
   of the file applies to the `n`-th input frame, in input order, and adds to
   a ROI side data offset when a frame carries both. As in the libaom and
   SVT-AV1 branches, only the deltas are used; a record's frame type and
   baseline QP are not.
2. **Anything that would make the file inert fails at encoder open**, naming
   why: an unreadable or malformed file, `aq-mode=0` (x264 applies
   `quant_offsets` through adaptive quantization, and `-preset ultrafast`
   sets it to 0), or a block grid that is not the video's macroblock grid.
3. **`vmaf-tune`'s Go and Python saliency code pass `-qpfile` for libx264**,
   as they do for libaom. Stock FFmpeg refuses the option, so a saliency
   encode on it fails instead of running without the ROI.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Force the file's per-frame QP and type through `x264_picture_t` (`i_qpplus1`, `i_type`) too | Closest to x264's own `--qpfile` | Overrides rate control for every frame, which the saliency use does not want; libaom and SVT-AV1 ignore both fields | One meaning of the file across the three encoders |
| Drop `-qpfile` on libx264 and document stock-FFmpeg ROI side data only | No code | The tools write a file and have no channel to give frames side data from the command line | The option is the only command-line channel |
| Keep `x264_param_parse` and ship a libx264 with a qpfile parameter | None | No such libx264 exists; the claim ("x264 has honoured per-macroblock QP deltas since r2390") was never true of the library | Unverifiable |
| Warn and continue when `aq-mode=0` | Looser | An inert option on a saliency encode is the failure being fixed | Fail closed |

## Consequences

- **Positive**: `-qpfile` works on all three encoders; an encode that cannot
  honour it fails at open instead of running without it; the tools no longer
  depend on a libx264 parameter that does not exist.
- **Negative**: a saliency encode with libx264 needs the fork's FFmpeg (as
  libaom already did) and a preset that keeps adaptive quantization.
- **Neutral / follow-ups**: `ffmpeg-patches/test/qpfile_check.py` (five of its
  seven checks fail on the previous series, all pass now) guards it.
  Measured on six 576x324 frames at `-crf 30 -preset veryfast`: +12 on every
  macroblock gives 4186 bytes, none 20123, -12 gives 104468. The block
  size of the SVT-AV1 and libaom branches, and whether `-svtav1-params
  qp-file=` (a different file format) is the right channel there, are not
  decided here.

## References

- RC4 WP15 brief (FFmpeg audit, 2026-10-07): the maintainer asked for
  correctness findings of everything VMAFx touches in FFmpeg, the patched
  encoders included, to be fixed with a failing-first test (paraphrased).
- FFmpeg audit 2026-10-07, findings C-7 and P4-3.
- [ADR-0312](0312-ffmpeg-patches-vmaf-tune-integration.md).
