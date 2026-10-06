<!-- markdownlint-disable MD013 MD060 -->
# ADR-2125: The VMAFx FFmpeg filters and GStreamer element follow their frames, refuse what they cannot score, and parse specs in the library

- **Status**: Proposed
- **Date**: 2026-10-06
- **Deciders**: maintainer (RC4 work package 9; decisions Q-047, Q-048); RC4 WP9
- **Tags**: rc4, ffmpeg, gstreamer, api, zero-copy

## Context

[ADR-1852](1852-vmafx-api-redesign.md) (decisions D4, D6, D8) replaces the
`libvmaf*` FFmpeg filters with one `vmafx` filter on the VMAFx API, renames
`libvmaf_tune`, `vmaf_pre` and `-vmaf-profile`, and adds a native GStreamer
element. Its design (Research-2158 section 5) leaves open how the filters
treat frames of a backend they cannot import, how they size the hardware frame
pools their producers own, where the `model` and `feature` strings are parsed,
and how an encoder's output reaches the filter as device frames in one command
(#2138). The measurements behind this record are
[Research-2162](../research/2162-vmafx-ffmpeg-gstreamer-filters.md).

## Decision

1. **One filter, the backend follows the frames.** `vmafx` scores software
   frames on the CPU (or on the device `backend=` names, which uploads them)
   and imports device frames without a copy on their device. A table of
   backend slots (`ffmpeg-patches/src/vf_vmafx.c`) maps each hardware pixel
   format to an import: CUDA frames on CUDA, DRM PRIME frames on SYCL (Intel)
   or HIP (AMD) as dma-bufs; the Metal slot refuses by name until the filter
   imports VideoToolbox frames. Every hardware format is in
   the format list, so negotiation never inserts a scale: a frame type
   without an import is refused naming it, or downloaded with `import=host`.
2. **Host frames live on the CPU device.** Software frames are wrapped (or,
   for NV12 / P010 / P016, imported from host memory) without a copy on the
   CPU device whatever device the context scores on; a context on a GPU
   uploads them.
3. **Refuse, never fall back.** A frame the library cannot import is retried
   once after a host wait (decision D8, in the library) and then fails the
   graph naming the backend, input and extractor; nothing passes unscored and
   no score line follows a failure, at configuration or per frame.
4. **Frame pools are checked, not grown.** A filter cannot enlarge an
   upstream pool. The filter holds at most `vmafx_context_max_in_flight() + 1`
   frames of an input where it holds any (device import, or `metadata=1`);
   a fixed-size pool (VAAPI, QSV, D3D11, D3D12, DXVA2) smaller than that is
   refused before the first frame, naming the size and the option that sets it.
   The GStreamer element proposes the same number in its allocation query.
5. **Specs are parsed in the library.** `vmafx_model_load_spec()` and
   `vmafx_context_use_feature_spec()` (ABI 0.1.10) parse the `model` and
   `feature` strings for the filter, the element and any other surface, with
   upstream FFmpeg's spellings accepted; the option tables are generated from
   `core/api/vmafx.toml` (WP8).
6. **Loopback decoders decode on the GPU through a fork patch.** Patch `0024`
   lets `-hwaccel`, `-hwaccel_device` and `-hwaccel_output_format` precede
   `-dec`. It is carried by the fork's series only and refreshed with every
   FFmpeg release (decision Q-048).
7. **Renamed filters keep their results.** `vmafx_tune` gives
   `libvmaf_tune`'s recommendation for the same model (its default model is
   the library's), `vmafx_pre` writes `vmaf_pre`'s bytes, `-vmafx-profile`
   does what `-vmaf-profile` did.
8. **No Dolby Vision profile 5 handling.** Sources need converting outside the
   pipeline; VMAFx reads and applies no RPU data (decision Q-047,
   [ADR-1685](1685-post-1-0-embedding-zero-copy-milestone.md)).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One filter per backend (as `libvmaf_cuda`, `libvmaf_sycl`) | Each filter states its frames | Options and fixes duplicated per backend; the backend is a property of the frames already | ADR-1852 D4 chose one filter |
| Download unsupported hardware frames silently | Every graph runs | A hidden host copy per frame and a different execution path than asked | No silent fallback; `import=host` makes it explicit |
| Grow or own a frame pool in the filter | No user action | A second pool and a device copy per frame | Costs the zero-copy path; the refusal names the one number to change |
| Parse `model` / `feature` in each filter | No library change | Two parsers (FFmpeg, GStreamer) drift | HISS-19: one parser in the library |
| `hwupload` after a software decode for #2138 | Stock FFmpeg | The encoded frames cross the bus twice | Defeats device scoring of the encoder's output |
| Submit the loopback hwaccel change to FFmpeg first | No fork patch | Outside the release train | Maintainer chose fork-only (Q-048) |

## Consequences

- **Positive**: one filter and one element score every backend the library
  builds with the same options and the same numbers as the CLI; refusals name
  their cause; #2138 runs as one command with no host copy.
- **Negative**: the fork carries patch `0024` across FFmpeg releases; users of
  fixed-size pools must size them (`-extra_hw_frames`).
- **Neutral / follow-ups**: QSV frames on SYCL; Metal through the tester bundle; retire the
  `libvmaf*` filters (WP10); `learned_filter_v1` needs a `[1,1,H,W]` export
  before `vmafx_pre` or `vmaf_pre` can run it.

## References

- [ADR-1852](1852-vmafx-api-redesign.md) (D4, D6, D8),
  [ADR-2074](2074-vmafx-window-scores.md), [ADR-2090](2090-motion-window-incremental.md),
  [ADR-2023](2023-vmafx-cuda-device-frames.md), [ADR-1897](1897-vmafx-abi-0x-numbering.md),
  [ADR-1685](1685-post-1-0-embedding-zero-copy-milestone.md),
  [Research-2162](../research/2162-vmafx-ffmpeg-gstreamer-filters.md).
- `Q-048`: "Our series only" (the loopback-decoder hwaccel patch stays in
  `ffmpeg-patches/`, nothing is submitted upstream).
- `Q-047`: "Keep DV5 out".
- Request WP9-1 (window clock, `extra_hw_frames` from
  `vmafx_context_max_in_flight()` + 1) and WP9-2 (windows complete before the
  end of the stream).
