<!-- markdownlint-disable MD013 MD060 -->
# Research-2162: the VMAFx FFmpeg filters and GStreamer element (RC4 WP9)

- **Status**: Active
- **Workstream**: [ADR-2125](../adr/2125-vmafx-ffmpeg-gstreamer-filters.md)
- **Last updated**: 2026-10-06

Measurements and design choices behind the `vmafx`, `vmafx_tune` and
`vmafx_pre` FFmpeg filters, the `-vmafx-profile` option, hardware decoding for
loopback decoders, and the native GStreamer element. Context:
[Research-2158](2158-vmafx-api-redesign.md) section 5 (the filter design) and
section 7 (decisions D1 to D8); [ADR-1852](../adr/1852-vmafx-api-redesign.md).
User documentation: [ffmpeg.md](../usage/ffmpeg.md) and
[gstreamer.md](../usage/gstreamer.md).

## 1. What was measured

All values are from the Netflix 576x324 pair (48 frames, `yuv420p`, 24 fps) on
a CPU-only FFmpeg build unless a device is named. The reproducers are in
`ffmpeg-patches/test/vmafx_filter_check.py`.

| Measurement | Result |
| --- | --- |
| Default model (`vmaf_v1.0.16_3d0h`, reported as `vmaf`) | `VMAF score: 82.816060` |
| `model=version=vmaf_v0.6.1` | `76.667831`, equal to the `vmaf` CLI |
| `vmaf_v0.6.1neg` alone | `75.073658` |
| Frame path line, CPU run | `vmafx frames: 0 imported on the device, 96 host, 0 downloaded` (both inputs count) |
| Two named models in one pass | `vmaf` 76.667831 and `vmaf_neg` 75.073658 |
| Two unnamed models | refused: `the context already scores a model of this name` |
| `feature='psnr\|cambi=full_ref=true'` | adds `psnr_y/cb/cr`, `cambi`, `cambi_full_reference`, `cambi_source` |
| One-second windows, `pool=mean+min` | window 0: frames 0 to 23, mean 76.326687, min 71.174759; window 1: frames 24 to 47, `partial`, mean 77.008976, min 72.373287 |
| `vmafx_tune`, `vmaf_v0.6.1`, target 95 / 80 | `recommended_crf=27.2` / `33.2`; same line as `libvmaf_tune` |
| Encode and score (NVENC to NVDEC to `vmafx`, CUDA) | windows 86.989539 and 85.531429; mean 86.260484; 96 frames imported on the device, 0 host, 0 downloaded |
| Frame pool, Intel Arc A380, VAAPI, `threads=4`, `import=host`, `metadata=1` | holds N = 18 frames; a pool of 17 refused before any frame, a pool of 18 scores equal to the CLI |
| Same stream, no pool check, pool of 4 | failed mid-stream with `ENOMEM` after printing a partial score |

The window values equal the CLI's per-frame scores pooled offline
(`scripts/ci/vmafx_window_pooling.py`, check `windows`). A window over a VMAF
model completes one frame after its last frame because the motion features run
incrementally ([ADR-2090](../adr/2090-motion-window-incremental.md)).

The hold count follows `vmafx_context_max_in_flight()` in
`core/include/vmafx/context.h`: `N = R + 2*T*(R+1) + 1`, plus 1 on a device
backend, with `R` the frame retention and `T` the thread count.

### GStreamer element

The measured table of the native element (properties, messages and scores on
the same pair) is in [gstreamer.md](../usage/gstreamer.md); the element and the
filter share one engine, one option table (generated from
`core/api/vmafx.toml`) and the spec parsers `vmafx_model_load_spec()` and
`vmafx_context_use_feature_spec()`, so a `model` or `feature` string means the
same on both.

## 2. Alternatives considered

### 2.1 Frame-pool sizing for hardware frames

The library keeps imported frames until it releases them, and `metadata=1`
holds the main frames until their scores are final. A filter cannot enlarge the
frame pool of the filter or decoder upstream of it.

| Option | Cost | Verdict |
| --- | --- | --- |
| Compute N, compare with the fixed pool, refuse before the first frame naming both numbers and the option to raise | One check at configuration; the user changes one number | Chosen |
| Silently over-allocate a pool of our own | The filter would have to own a second pool of the decoder's format and copy into it: a device copy per frame, and a hidden memory cost | Rejected |
| Copy every frame to host memory and release the pool frame at once | Costs the zero-copy path the filter exists for; `import=host` offers it explicitly | Kept as the opt-in, not the default |
| Do nothing | A pool of 4 failed mid-stream with `ENOMEM` after a partial score | Rejected |

Pools that grow (CUDA, Vulkan) are not limited, so the check applies only to
fixed-size pools (VAAPI, QSV, D3D11, D3D12, DXVA2 with `initial_pool_size`).

### 2.2 Hardware decoding in loopback decoders (#2138)

Stock FFmpeg reads `-hwaccel`, `-hwaccel_device` and `-hwaccel_output_format`
for input streams only, so the decoder behind `-dec` returns system memory.

| Option | Cost | Verdict |
| --- | --- | --- |
| Fork patch `0024` that reads the three options before `-dec` (loopback decoders exist since FFmpeg 7.0) | A patch refreshed with every FFmpeg release | Chosen (decision Q-048) |
| `hwupload` after a software decode | Host decode and an upload per frame: the encoded frames cross the bus twice | Rejected |
| Submit the change to FFmpeg first | Review time outside the release train; the filter works only once it is merged | Not now: the maintainer chose fork-only (Q-048) |

## 3. Open items

- The SYCL and HIP slots import DRM PRIME frames (VAAPI frames through
  `hwmap`); QSV frames and the Metal slot (VideoToolbox) still refuse by name,
  so `libvmaf_sycl` and `libvmaf_metal` stay for those frames until then.
- VideoToolbox frames are verified through the tester bundle; no macOS device
  run is part of this measurement.
- `vmafx_pre` refuses the shipped `learned_filter_v1` model: its ONNX input is
  `[batch,1,224,224]` and the pre-filter path takes a static `[1,1,H,W]` model
  (state row `T-VMAFX-PRE-LEARNED-FILTER-V1-REFUSED-2026-10-06`).
- The `libvmaf*` filters and patches retire in WP10; patch `0024` stays carried
  by the fork.
