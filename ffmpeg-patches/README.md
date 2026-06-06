<!-- markdownlint-disable MD013 -->
# ffmpeg-patches/

Local patches against FFmpeg **n8.1.1** for integrating this VMAF fork into
`libavfilter/vf_libvmaf*` plus a new `vf_vmaf_pre` filter.

## Contents

- **`0001-libvmaf-add-tiny-model-option.patch`** — adds `tiny_model` /
  `tiny_device` / `tiny_threads` / `tiny_fp16` options on the existing
  `libvmaf` filter; calls `vmaf_use_tiny_model()` when set.
- **`0002-add-vmaf_pre-filter.patch`** — new `vmaf_pre` filter: luma-in /
  luma-out learned pre-processing via `vmaf_dnn_session_*`. Chroma
  planes pass through.
- **`0003-libvmaf-wire-sycl-backend-selector.patch`** — adds `sycl_device`
  / `sycl_profile` options on the `libvmaf` filter; invokes
  `vmaf_sycl_state_init()` + `vmaf_sycl_import_state()` when
  `sycl_device >= 0`.
- **`0004-libvmaf-wire-vulkan-backend-selector.patch`** — adds
  `vulkan_device` option on the `libvmaf` filter; wires
  `vmaf_vulkan_state_init` + the deferred-pool path from ADR-0238.
- **`0005-libvmaf-add-libvmaf-sycl-filter.patch`** — registers a
  dedicated `libvmaf_sycl` filter for zero-copy VAAPI/QSV import
  (consumes `AVFrame->data[3] -> mfxFrameSurface1*`).
- **`0006-libvmaf-add-libvmaf-vulkan-filter.patch`** — registers a
  dedicated `libvmaf_vulkan` filter for zero-copy VkImage import per
  [ADR-0186](../docs/adr/0186-vulkan-image-import-impl.md).
- **`0010-libvmaf-wire-cuda-backend-selector.patch`** — adds a
  `cuda` boolean option on the `libvmaf` filter and the
  `--enable-libvmaf-cuda` configure flag. When `cuda=1` the filter
  inits a `VmafCudaState` on the primary CUDA context (device picked
  by `CUDA_VISIBLE_DEVICES`), imports it via `vmaf_cuda_import_state`,
  and dispenses `VmafPicture`s from a HOST_PINNED preallocation pool
  so software AVFrame input flows into pinned-host memory the CUDA
  feature kernels DMA from without a staging copy. Mirrors the
  SYCL/Vulkan selector pattern and runs alongside the upstream
  dedicated `libvmaf_cuda` filter (which keeps its own `cu_state`
  for hwaccel CUDA frames in). See
  [ADR-0350](../docs/adr/0350-ffmpeg-libvmaf-cuda-backend-selector.md).
- **`0011-libvmaf-wire-hip-backend-selector.patch`** — adds a
  `hip_device` integer option on the `libvmaf` filter and the
  `--enable-libvmaf-hip` configure flag. When `hip_device >= 0` the
  filter inits a `VmafHipState` on the selected AMD ROCm/HIP device,
  imports it via `vmaf_hip_import_state`, and frees the state after
  `vmaf_close()`. Completes the SYCL / Vulkan / CUDA / HIP selector
  symmetry on the `libvmaf` filter. A dedicated `libvmaf_hip` filter
  for ROCm hwdec zero-copy import is deferred until FFmpeg exposes a
  ROCm/HIP hardware-frame context (no `ffhipcodec` equivalent of
  `ffnvcodec` exists at this time). See
  [ADR-0376](../docs/adr/0376-ffmpeg-patches-hip-backend-selector.md).- **`0013-libvmaf-add-libvmaf-metal-filter.patch`** — registers a
  dedicated `libvmaf_metal` filter for VideoToolbox hwdec zero-copy
  import. Consumes `AVFrame->format == AV_PIX_FMT_VIDEOTOOLBOX`,
  pulls the IOSurface backing each `CVPixelBufferRef` via
  `CVPixelBufferGetIOSurface`, and routes it through
  `vmaf_metal_picture_import` / `vmaf_metal_read_imported_pictures`.
  Working IOSurface → `VmafPicture` import on Apple-Family-7+ hosts
  via `IOSurfaceLock` + memcpy (ADR-0423 / T8-IOS-b). The filter
  passes `device=0, command_queue=0` so libvmaf falls back to
  `MTLCreateSystemDefaultDevice` until FFmpeg exposes an
  `AVMetalDeviceContext`; non-Apple-Silicon hosts get a clear
  `-ENODEV` from `vmaf_metal_state_init_external`. Companion to 0012
  (`metal_device` option
  on the regular libvmaf filter) — together they give users
  `libvmaf=metal_device=N` (software AVFrame input + Metal compute)
  and `libvmaf_metal=...` (VideoToolbox hwdec + zero-copy import). See
  [ADR-0423](../docs/adr/0423-metal-iosurface-import-scaffold.md).
- **`0016-libvmaf-wire-score-fmt-on-all-vmaf-filters.patch`** — adds a
  `score_fmt` AVOption (string, default `NULL` = `"%.6f"`) to all four
  vmaf filters (`libvmaf`, `libvmaf_sycl`, `libvmaf_vulkan`,
  `libvmaf_metal`). Replaces `vmaf_write_output()` with
  `vmaf_write_output_with_format()` so callers can request full-precision
  IEEE-754 lossless output (`score_fmt=%.17g`), equivalent to the
  `--precision=max` CLI flag. Wires the fork-added
  `vmaf_write_output_with_format()` C API (ADR-0119 / commit 695d29626).
  Symmetric with the `cpumask`/`gpumask` options added in ADR-0576 (patch
  0014).

Every patch is guarded by `check_pkg_config` so it degrades gracefully when
libvmaf was built without the relevant feature (`-Denable_dnn`, `-Denable_sycl`,
`-Denable_vulkan`, `-Denable_cuda`, `-Denable_hip`).

## What works without a patch

Any feature extractor registered in the fork's libvmaf — including
**SSIMULACRA 2**, PSNR-HVS, VIF, ADM, MS-SSIM, motion, CAMBI, etc. — is
already reachable through upstream FFmpeg's stock `libvmaf` filter via
its `feature` option. No patch needed:

```bash
ffmpeg -i ref.mp4 -i dist.mp4 \
  -lavfi "[0:v][1:v]libvmaf=feature='name=ssimulacra2'" \
  -f null -
```

The same `feature=` syntax accepts per-extractor options:

```bash
# BT.709 full-range matrix
feature='name=ssimulacra2:yuv_matrix=2'
```

Works with both the stock `libvmaf` filter and, via CUDA, the
`libvmaf_cuda` filter (SSIMULACRA 2 currently has no CUDA path; it falls
back to CPU SIMD — AVX-512 / AVX2 / NEON per ADR-0161 / 0162 / 0163).

The patches in this directory only cover fork-added surfaces that
DO NOT fit the generic `feature=` plumbing: the DNN session API
(`tiny_model`), the learned pre-processing filter (`vmaf_pre`), the
SYCL / Vulkan backend selectors on the `libvmaf` filter, and the
dedicated `libvmaf_sycl` / `libvmaf_vulkan` filters for zero-copy
hardware-frame import.

## How to apply

```bash
cd /path/to/ffmpeg    # must be at tag n8.1.1
for p in /path/to/vmaf/ffmpeg-patches/000*-*.patch; do
    git am --3way "$p" || break
done
```

Or via the helper skill: `/ffmpeg-apply-patches /path/to/ffmpeg`.

> **Verification gate (CLAUDE.md §12 r14)**: patches `0002` through
> `0006` build on each other (each assumes the cumulative state of
> every earlier patch). Per
> [ADR-0118](../docs/adr/0118-ffmpeg-patch-series-application.md) and
> [ADR-0186](../docs/adr/0186-vulkan-image-import-impl.md), the
> verification gate for any change touching a libvmaf C-API surface
> the patches consume is a **series replay** against a pristine
> `n8.1.1` checkout (cumulative `git am --3way`), NOT a per-patch
> `git apply --check`. The latter rejects `0002+` because they
> reference cumulative-state hunks that don't exist in pristine
> `n8.1.1`. **Pending verification for patch 0016 (score_fmt, 2026-06-06)**:
> patches 0001-0015 were last verified against n8.1.1 on 2026-05-20
> (ADR-0643); patch 0016 has not yet been replay-verified — a series replay
> of all 16 patches against pristine n8.1.1 is required before merge. The
> most recent full no-drift verification is
> the encoder-profile hand-off (ADR-0643, 2026-05-20): all 15 patches
> (0001-0015) apply cleanly against pristine `n8.1.1`, the latest
> released FFmpeg 8.x.x tag verified on 2026-05-20, with zero conflicts.
> The previous verification was the full-feature-exposure
> sync (ADR-0576, 2026-05-18) against pristine `n8.1.1`; before that was
> the n8.1 to n8.1.1 base
> bump (2026-05-09); before that was
> [ADR-0277 (2026-05-04)](../docs/adr/0277-ffmpeg-patches-refresh-2026-05-04.md);
> the procedure is captured in
> [`docs/rebase-notes.md`](../docs/rebase-notes.md) under the same
> ADR heading.

## How to smoke-test

```bash
bash ffmpeg-patches/test/build-and-run.sh
```

Requires `libvmaf` to be installed (`pkg-config --cflags libvmaf` must
resolve). Set `VMAF_PREFIX` to point at a non-standard install prefix.
Pins `FFMPEG_SHA=n8.1.1`; override to test against a newer tag.

## How to regenerate

After editing FFmpeg locally, run `/ffmpeg-build-patches` to diff against
the tracked upstream base and rewrite the numbered patches in place.

## License

BSD-3-Clause-Plus-Patent for patches authored in this repo; patches are
applied against FFmpeg (LGPL / GPL) — the resulting linked binary's
distribution terms are governed by FFmpeg's license, not this one.

## vmaf-tune integration patches (ADR-0312)

The 0007–0009 patches plus 0015 wire the in-tree `tools/vmaf-tune/`
orchestrator into FFmpeg's encoder-side and CLI-side surfaces:

- **`0007-libvmaf-tune-qpfile-unified.patch`** — adds an `-qpfile <path>`
  AVOption to `libx264`, `libsvtav1`, and `libaom-av1`. The shared
  parser at `libavcodec/qpfile_parser.{c,h}` reads vmaf-tune's
  `saliency.py` qpfile format once; libx264 forwards it to x264's
  native per-MB qpfile reader; SVT-AV1 wires it through to the
  per-picture `ROI_MAP_EVENT` priv-data ABI (gated on SVT-AV1 ≥ 1.6.0;
  older releases log-and-continue); libaom-av1 wires it through to
  `aom_codec_control(AOME_SET_ROI_MAP, ...)` per frame, with up to 8
  segment QPs and uniform binning when the qp_offset value span
  exceeds the segment budget.
- **`0008-add-libvmaf_tune-filter.patch`** — new `libvmaf_tune` 2-input
  filter (`libavfilter/vf_libvmaf_tune.c`). Scaffold: pass-through on
  the main pad, framesync ref pad, AVOptions
  (`recommend_target_vmaf` / `recommend_crf_min` / `recommend_crf_max`),
  and a final-line `recommended_crf=…` log at uninit. Linear
  CRF↔VMAF interpolation; the full Optuna TPE recommend loop stays
  in `tools/vmaf-tune/src/vmaftune/recommend.py`.
- **`0009-pass-autotune-cli-glue.patch`** — adds a `-pass-autotune`
  flag to `fftools/ffmpeg_opt.c` that emits an advisory log line
  pointing at `docs/usage/vmaf-tune-ffmpeg.md`. Glue only — real
  orchestration stays in vmaf-tune.
- **`0015-vmaf-tune-profile-cli-glue.patch`** — adds a
  `-vmaf-profile <path>` flag to `fftools/ffmpeg_opt.c` that emits an
  advisory hand-off to `vmaf-tune encode-profile --profile <path>`.
  Glue only — the Python tool reads the report JSON/HTML/Markdown,
  selects one recommendation via `--codec` / `--target-vmaf` /
  `--recommendation-index`, and then invokes FFmpeg with the codec
  adapter registry.

### vmaf-tune patch invariant

Patches **0007–0009** must keep the qpfile parser shared at
`libavcodec/qpfile_parser.{c,h}`. New encoder adapters that grow a
`-qpfile` AVOption inherit the same parser; do not fork it. When
`tools/vmaf-tune/src/vmaftune/saliency.py`'s `write_x264_qpfile`
output format changes, patch 0007's parser must change in the same
PR (CLAUDE.md §12 r14).

Patch **0015** must remain advisory. The profile reader and selection
logic live in `vmaf-tune encode-profile`; duplicating that JSON schema
inside FFmpeg would make the patch stack rebase-sensitive for no
encoding benefit.
