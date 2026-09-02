# Research 0662: Vulkan Motion Lavapipe Parity

## Question

Can the always-on CPU↔Vulkan/lavapipe parity matrix cover the integer motion
family again without weakening tolerances?

## Findings

- `motion_v2_vulkan` drifted from CPU on the Netflix normal pair with
  `max_abs_diff = 2.624e-3` for both
  `VMAF_integer_feature_motion_v2_sad_score` and
  `VMAF_integer_feature_motion2_v2_score`.
- CPU `integer_motion_v2.c::mirror` maps the high edge as
  `2 * size - idx - 2`. The CUDA, SYCL, and Vulkan `motion_v2` kernels
  instead used `2 * size - idx - 1`, matching stale ADR-0193 prose rather than
  the current CPU reference.
- Direct `motion_vulkan` still crashes on lavapipe in the Mesa llvmpipe driver,
  but the newer `integer_motion_vulkan` twin completes and matches CPU.
- `integer_motion_vulkan` had `debug=false` by default, unlike CPU, CUDA, and
  legacy Vulkan motion, so parity could not compare the raw `integer_motion`
  metric until that default was restored.

## Validation

All commands ran inside the `vmaf-dev-mcp` container unless noted otherwise.

```bash
VK_LOADER_DRIVERS_SELECT="*lvp*" \
LD_LIBRARY_PATH=/tmp/vmaf-build-vulkan-lvp/src \
.venv/bin/python scripts/ci/cross_backend_vif_diff.py \
  --vmaf-binary /tmp/vmaf-build-vulkan-lvp/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --feature motion_v2 --backend vulkan --places 4
```

Result: both `motion_v2` metrics had `max_abs_diff = 0.0`.

```bash
VK_LOADER_DRIVERS_SELECT="*lvp*" \
LD_LIBRARY_PATH=/tmp/vmaf-build-vulkan-lvp/src \
.venv/bin/python scripts/ci/cross_backend_vif_diff.py \
  --vmaf-binary /tmp/vmaf-build-vulkan-lvp/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --feature motion --backend vulkan --places 4
```

Result: `integer_motion`, `integer_motion2`, and `integer_motion3` all passed
with `max_abs_diff = 1e-6`.

```bash
VK_LOADER_DRIVERS_SELECT="*lvp*" \
LD_LIBRARY_PATH=/tmp/vmaf-build-vulkan-lvp/src \
.venv/bin/python scripts/ci/cross_backend_parity_gate.py \
  --vmaf-binary /tmp/vmaf-build-vulkan-lvp/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --backends cpu vulkan \
  --features adm ciede float_adm float_ansnr float_moment motion motion_v2 \
             float_motion float_ms_ssim float_ms_ssim_lcs float_psnr \
             float_ssim float_vif psnr psnr_hvs ssimulacra2 vif \
  --gpu-id "vulkan:0x10005:0x0" \
  --json-out /tmp/parity-gate-report/parity.json \
  --md-out /tmp/parity-gate-report/parity.md
```

Result: full lavapipe matrix passed with `motion` and `motion_v2` included.

## Limitation

An ad hoc CUDA+SYCL+Vulkan mixed build in `/tmp` generated the edited
`motion_v2_score.fatbin` and `integer_motion_v2_sycl.o`, but failed at final
link with pre-existing Vulkan private-symbol unresolved references in that LTO
configuration. The focused Vulkan build and parity gate passed.
