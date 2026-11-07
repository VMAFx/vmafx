<!-- markdownlint-disable MD060 -->
# HIP backend history

This page keeps the dated record of how the HIP backend reached its current
state, newest entry first. Each entry says what was true then and, where it
no longer holds, what replaced it. The current behaviour is on the
[overview](overview.md), the [twins page](twins.md) and the
[uploads page](uploads.md); open items are in
[`docs/state.md`](../../state.md).

## 2026-10-01: named twins and `--threads`

A twin named with `--feature` runs on the thread that calls
`vmaf_read_pictures()`, with or without `--threads`. Until this date
`adm_hip` and `float_vif_hip`, then the two twins `--backend hip` did not select
for their CPU names, failed with `problem flushing context` whenever
`--threads` was given, because the worker pool tried to run them.

## 2026-10-01: first device run of the RC3 parity changes

Three RC3 changes brought HIP twins onto the CPU's arithmetic: the diff-first
motion kernel ([ADR-1377](../../adr/1377-hip-motion-diff-first.md)), the tiny
frame guards ([ADR-1381](../../adr/1381-hip-integer-tiny-frame-guards.md)) and
the CPU option tables
([ADR-1382](../../adr/1382-hip-twin-cpu-option-parity.md)). They build for
gfx90a, gfx1030, gfx1036 and gfx1100 and pass every device-free check. On
2026-10-01 they passed on a gfx1036 (Ryzen 9950X3D iGPU, ROCm 7.2.4): every
HIP device test OK, `motion_hip` equal to the CPU `motion` on every frame, and
the parity gate within its tolerances.

### Measured on a gfx1036 after the RC3 parity change

| Check | Result |
|---|---|
| HIP device tests (`--suite gpu`, 52 tests) | all OK; the only skip marker was `test_hip_float_ssim_parity_large`, because `float_ssim_hip` did not decimate yet (it does since ADR-1405, and the test runs) |
| `motion_hip` against `--backend cpu`, Netflix 576x324 pair | `motion2` / `motion3` identical (1.26e-5 apart before) |
| Parity gate, `float_ssim float_ssim_lcs psnr motion_v2 vif` | every cell OK: 1.0e-5, 1.0e-5, 0, 0, 1.0e-6 |
| `psnr_hip` with all four CPU options | every per-frame value and every `apsnr_*` identical to the CPU |
| `float_motion_hip` on 3x3 and 17x17 frames | exits 0, within 4e-6 of the CPU |
| BBB 4K, ms per frame (master / this change) | `motion_hip` 14.25 / 12.95, `motion_v2_hip` 10.17 / 13.24 |

`motion_v2_hip` got slower on this iGPU because of the staged upload, not
the kernel: with the waiting `vmaf_hip_picture_upload()` the same kernel
runs at 10.70 ms per frame, and `motion_hip` at 10.75. On an integrated GPU
the runtime copies a pageable picture without an extra host copy, so the
staged path's copy into pinned memory costs more than the wait it saves.

`T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19` in [`docs/state.md`](../../state.md)
has the numbers, including runs with several twins, where the two uploads are
within noise. Since ADR-1408 both motion twins read the shared planes through
the waiting upload again; see [Picture uploads](uploads.md#picture-uploads).

## 2026-09-30: CAMBI and SpEED move onto the device

`cambi_hip` ([ADR-1378](../../adr/1378-hip-cambi-device-resident.md)) and
`speed_chroma_hip` / `speed_temporal_hip`
([ADR-1384](../../adr/1384-hip-speed-device-resident.md)) stopped running any
stage of the CPU extractors on the host. On that date they built for the four
default targets but had not run on an AMD device. They have since: 178 of 178
`cambi` frames, 759 of 759 `speed_chroma` values and 256 of 256
`speed_temporal` values are identical to the CPU
([agreement table](overview.md#agreement-with-the-cpu)). Current behaviour is
in [twins](twins.md#cambi_hip-speed_chroma_hip-and-speed_temporal_hip).

## 2026-09-27: measured parity with the rc.1 image

With the `v1.0.0-rc.1-rocm10` image (published and deleted after 0.1.0,
[ADR-3398](../../adr/3398-delete-withdrawn-rc-releases.md)) on AMD gfx1036
(the Radeon iGPU of a Granite Ridge CPU), the Netflix golden src01 pair and
`vmaf_v0.6.1`, HIP scored 76.667848 against the CPU's 76.667831, a pooled
difference of 1.67e-5.
Per frame, `motion2` and `motion3` differed by up to 1.26e-5 and VIF by up to
5.4e-7, and ADM ran on the CPU. This was close agreement, not bit-exactness,
and it was inside the 5e-5 `places=4` cross-backend gate from
[ADR-0214](../../adr/0214-gpu-parity-ci-gate.md).

The motion difference came from `motion_hip` blurring each frame instead of
the frame difference. Since 2026-09-30 it runs the CPU's arithmetic and, on
the same gfx1036 on 2026-10-01, matched the CPU exactly. VIF was then the
only difference: 76.667849 against 76.667831, 1.79e-5 pooled and at most
4.9e-5 per frame, slightly more than before because the old motion error
partly offset the VIF one.

Superseded: `vif_hip` has returned the CPU's scores bit for bit since
[ADR-1435](../../adr/1435-hip-vif-cpu-log2-table.md), and every other twin
except `ciede_hip` is exact too. The pooled score was not re-measured for
this page.

## 2026-09-19: asynchronous upload race fixed

HIP extractors used to upload the host pictures with an asynchronous copy and
return without waiting for it. On a multi-frame run the picture buffer was
refilled with the next frame while the copy still read it, so frames were
scored against the next frame's samples: a different set on every run with
one extractor, and the same 46 of 48 frames on every run with several in one
process. It affected `ciede_hip`, `float_adm_hip`, `float_moment_hip`,
`float_psnr_hip`, `float_ssim_hip`, `float_vif_hip`, `psnr_hip` and `vif_hip`
(up to 10.7 dB on `float_psnr`, 0.30 on `vif`).

Scores from a multi-frame HIP run made before this fix should be recomputed.
Every upload now waits until it has read the picture; see
[Picture uploads](uploads.md#picture-uploads).

## 2026-09-18: 18 of 19 extractors active

18 of the 19 registered HIP extractors carried active GPU flags
(`VMAF_FEATURE_EXTRACTOR_HIP` / `VMAF_FEATURE_EXTRACTOR_TEMPORAL`) and executed
on AMD GPU hardware, each validated by a device parity test against the CPU
reference.

`integer_ssim_hip` joined them on this date. Its kernel used to be an 11-tap
float Gaussian, 4.5e-3 away from the CPU `ssim`, so it was kept out of
dispatch (ADR-0564). It then ran the CPU's 9-tap int64 kernel, ported from
the CUDA twin; see [integer_ssim_hip](twins.md#integer_ssim_hip).

The remaining extractor, `adm_hip` (then called `integer_adm_hip` in this
entry), kept `.flags = 0`, so a run silently fell back to the CPU for it.

At the time it lacked internal HtoD picture staging buffers and passed host
pointers directly into device kernels, and the plan was to wait for picture
staging (about 350 lines) or the HIP device picture pool (T7-10c, about 600
lines). Since then it uploads through the shared planes and returns the CPU's
values bit for bit ([ADR-1423](../../adr/1423-hip-adm-cpu-row-rounding.md)),
and it still carries `.flags = 0`: it runs when named, and the default
model's `adm3` and `aim` use the CPU ([known gaps](overview.md#known-gaps)).

## 2026-06-13: ADR-1103, `vif_hip` boundary fix

After the ADR-0563 carry-bit fix, `integer_vif_hip` still produced a residual
parity gap of places~2.75 (max |HIP−CPU| ≈ 0.0018 per scale) on the Netflix
src01 576×324 pair. The root cause was a boundary-condition mismatch: all
filter-loop reads used `clamp_i` (replicate-edge), while the CPU reference uses
a **symmetric reflect** (`PADDING_SQ_DATA` in `integer_vif.h`) and the CUDA twin
uses a "two-bounce mirror" in its shared-memory load stage.

The fix replaces `clamp_i` with `mirror2_i` in all six filter-loop reads in
`vif_statistics.hip`. Verification on gfx1030 (RDNA2, wave32):

| Scale | Max |HIP−CPU| (post-fix) | Places |
|-------|------------------------|--------|
| scale0 | 0.0000010 | ~6.00 |
| scale1 | 0.0000010 | ~6.00 |
| scale2 | 0.0000010 | ~6.00 |
| scale3 | 0.0000010 | ~6.00 |

See [ADR-1103](../../adr/1103-hip-vif-mirror2-boundary.md).

This reached `places=4`; since
[ADR-1435](../../adr/1435-hip-vif-cpu-log2-table.md)
the twin is bit-identical (see [vif_hip](twins.md#vif_hip)).

## 2026-05-18: integer ADM kernels

The four ADM kernels (`adm_dwt2`, `adm_csf`, `adm_csf_den`, `adm_cm`)
that the ADR-0537 sub-bundle had left as weak HSACO fallbacks now build
standalone via `hipcc --genco` and are registered in `hip_kernel_sources`.
The xxd-embedded strong symbols replace the weak slots in
`hip_hsaco_stubs.c` (which is now ADM-stub-free).

End-to-end on AMD gfx1036 inside `vmaf-dev-mcp`:

```bash
docker exec vmaf-dev-mcp vmaf \
    --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend hip --feature adm --json --output /tmp/adm_hip.json
```

Bit-exact vs CPU on the Netflix golden src01 pair (delta = 0.000000):

| Feature | CPU | HIP | Diff |
|---|---|---|---|
| `integer_adm`        | 0.934506 | 0.934506 | 0.000000 |
| `integer_adm2`       | 0.934506 | 0.934506 | 0.000000 |
| `integer_adm3`       | 0.953973 | 0.953973 | 0.000000 |
| `integer_adm_scale0` | 0.907897 | 0.907897 | 0.000000 |
| `integer_adm_scale1` | 0.893864 | 0.893864 | 0.000000 |
| `integer_adm_scale2` | 0.929998 | 0.929998 | 0.000000 |
| `integer_adm_scale3` | 0.964951 | 0.964951 | 0.000000 |

The CUDA twin's per-warp `__shfl_down_sync` reduction
(`cuda_helper.cuh::warp_reduce`) is replaced by per-thread `atomicAdd` on
the 64-bit unsigned accumulator. AMD wavefronts are 64 wide (not the 32
the CUDA shuffle mask hard-codes); per-thread atomicAdd is **bit-exact**
since uint64 addition is associative and commutative. Same pattern
`vif_statistics.hip` adopted (ADR-0537).

See [ADR-0539](../../adr/0539-hip-adm-kernels-real.md).

## 2026-05-18: integer VIF kernel fix and moment registration

### ADR-0537: integer_vif_hip kernel fix

The integer VIF HIP extractor now runs end-to-end on AMD gfx1036 inside
the `vmaf-dev-mcp` container:

```bash
docker exec vmaf-dev-mcp vmaf \
    --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --backend hip --feature vif_hip --json --output /tmp/vif_hip.json
```

Reports the four VIF scale scores within `places=3` of CPU on the Netflix
golden pair.  The `places=4` parity target is tracked as an ADR-0537
follow-up — the residual ~0.001–0.003 per-scale delta comes from the
kernel's edge-clamp boundary vs CPU's pre-padded mirror boundary
(cumulative across downsamples).

#### Defects fixed

Four defects fixed (see
[ADR-0537](../../adr/0537-hip-integer-vif-kernel-fix.md)):

1. The 4×18 `vif_filter1d_table` is uploaded to a device buffer at init
   (the pre-fix kernel was handed a host pointer that the GPU faulted on).
2. Filter half-widths corrected from `{9,5,3,0}` (parsed from the kernel-
   name suffix — the wrong number) to `{8,4,2,1}` (= `vif_filter1d_width
   [scale] / 2`).  Pre-fix read 19/11/7/1 coefficients per output pixel
   from an 18-entry table.
3. Added the rd-filter downsample-write path so scales 1–3 read the half-
   resolution planes the previous horizontal pass produced.  Pre-fix left
   them uninitialised.
4. Picture buffers are staged into device memory via `hipMemcpy2DAsync`
   before scale-0 reads them (mirrors the `integer_motion_hip.c` pattern).

#### Adjacent fixes

Adjacent fixes bundled in the same PR:

- Missing HSACO entries (`motion_score`, `ms_ssim_score`, `psnr_hvs_score`,
  `integer_ssim_score`, `float_vif_score`, `ssimulacra2_blur`,
  `ssimulacra2_mul`) added to `hip_kernel_sources` — ADR-0533 wired the
  extractor registration sweep but not the corresponding kernel compilation.
- Weak-stub TU `hip_hsaco_stubs.c` provides empty fallback `_hsaco`
  symbols for the four ADM kernels (`adm_dwt2`, `adm_csf`, `adm_csf_den`,
  `adm_cm`) that don't yet build standalone via `hipcc --genco` because
  they reference CUDA-specific helper macros.  As individual kernels
  port to standalone-buildable `.hip` sources, their weak-stub line is
  deleted from `hip_hsaco_stubs.c` in the same PR — ADR-0539 establishes
  that pattern, starting with `float_vif_score_hsaco` whose real kernel
  has shipped at `core/src/feature/hip/float_vif/float_vif_score.hip`
  since ADR-0379 / PR #1025.
- `hipcc --genco` include path adds `meson.current_build_dir()` +
  `feature/hip` + `hip` so kernel sources can resolve `config.h` /
  `integer_*_hip.h` headers.

Re-enables `VMAF_FEATURE_EXTRACTOR_HIP` on `vmaf_fex_integer_vif_hip` —
ADR-0530 had cleared it pending this fix.

Superseded: the `places=4` follow-up named above is the boundary fix of
[ADR-1103](#2026-06-13-adr-1103-vif_hip-boundary-fix) and, later, the exact
twin of ADR-1435. After ADR-0539 ported the ADM kernels, the ADR-0536 weak-stub
path in `hip_hsaco_stubs.c` backs no extractor: the four ADM stubs were removed
and the file's header says no other extractor needs one.

### ADR-0539: integer_moment HIP kernel registration

This entry closed the last unresolved-symbol gap in the `enable_hipcc=true`
build by registering an `integer_moment_score` kernel key next to the
existing `moment_score` key. That registration no longer exists: the tree has
no `integer_moment_hip.c` and `hip_kernel_sources` in `core/src/meson.build`
has only the `moment_score` key (`hip/float_moment/moment_score.hip`), which
`float_moment_hip.c` consumes. The check below was made while it existed.

End-to-end verification on the Netflix `src01_hrc00 ↔ src01_hrc01`
576×324 pair (HIP vs CPU, `--backend hip|cpu --feature
psnr|psnr_hvs|float_moment`):

| Feature              | HIP                     | CPU                     | delta |
|----------------------|-------------------------|-------------------------|-------|
| `psnr_y` mean        | 30.755064               | 30.755064               | 0.000000 |
| `psnr_cb` mean       | 38.449441               | 38.449441               | 0.000000 |
| `psnr_cr` mean       | 40.991910               | 40.991910               | 0.000000 |
| `psnr_hvs` mean      | 31.330446               | 31.330446               | 0.000000 |
| `psnr_hvs_y` mean    | 30.578766               | 30.578766               | 0.000000 |
| `psnr_hvs_cb` mean   | 37.258498               | 37.258498               | 0.000000 |
| `psnr_hvs_cr` mean   | 38.200260               | 38.200260               | 0.000000 |
| `float_moment_ref1st` mean | 59.788567         | 59.788567               | 0.000000 |
| `float_moment_dis1st` mean | 61.332007         | 61.332007               | 0.000000 |
| `float_moment_ref2nd` mean | 4696.668388       | 4696.668388             | 0.000000 |
| `float_moment_dis2nd` mean | 4798.659574       | 4798.659574             | 0.000000 |

All within places=4 of CPU (in fact bit-exact: delta=0.000000).

## 2026-05-18: dispatch posture and `--backend hip`

[ADR-0519](../../adr/0519-hip-import-state-implementation.md) promoted the
library-side `vmaf_hip_import_state` from `-ENOSYS` to a real implementation,
so `vmaf --backend hip` ran end to end on any AMD GPU visible to ROCm and
produced a valid VMAF JSON. HIP joined CUDA, SYCL and Metal as a fully working
runtime-selected backend. (The Vulkan backend was removed in ADR-0726.)

[ADR-0530](../../adr/0530-hip-feature-flag-promotion-and-picture-buffer.md)
then set `VMAF_FEATURE_EXTRACTOR_HIP` on `vmaf_fex_integer_motion_hip`, so the
model-driven dispatch selects it when a HIP state has been imported (`vmaf
--backend hip` implies that import). The `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`
enum entry was added for a future HIP picture pool; pictures still arrive as
`VMAF_PICTURE_BUFFER_TYPE_HOST` and the HIP feature TUs perform their own
HtoD copies (`hipMemcpy2DAsync`).

End-to-end check at the time: `--backend hip --feature integer_motion`
produced a clean VMAF JSON with VMAF = 76.71 on the Netflix src01 pair
against 76.67 on the CPU, a 0.04 gap that was outside the 5e-5 `places=4`
cross-backend gate of [ADR-0214](../../adr/0214-gpu-parity-ci-gate.md). 48
`hipModuleLaunchKernel(calculate_motion_score_kernel_8bpc)` launches per
48-frame clip confirmed that the HIP kernel was dispatching. The later
motion kernel ([ADR-1377](../../adr/1377-hip-motion-diff-first.md)) and the
[agreement table](overview.md#agreement-with-the-cpu) supersede that gap.
