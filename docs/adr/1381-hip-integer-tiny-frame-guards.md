<!-- markdownlint-disable MD013 MD060 -->
# ADR-1381: HIP tile loads and the ADM vertical DWT clamp their rows; vif_hip hands frames below 16 pixels to the CPU

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu-parity, correctness, adm, vif, motion, fork-local

## Context

On an Arc B580 and a UHD 770 the SYCL twins read outside their device buffers on small frames (`T-SYCL-TILE-HALO-OOB-READ-2026-09-29`, `T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29`, fixed in #1622): a tiled kernel loads a fixed tile for every thread, padding threads included, and reflects each index once, which leaves the plane for padding rows of a plane smaller than the tile; and integer VIF reflects each filter tap once per scale, which stays inside the plane only from 16 pixels up. Two rows asked the same questions of the HIP twins without an AMD device to answer them:

- `T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29`: `adm_dwt2_load_column()` in `core/src/feature/hip/integer_adm/adm_dwt2.hip` reflects the bottom edge once, fixes only the first row with `abs()`, and loads before any output-row check. Replaying the launched grid on the host (Research-1377) shows the single reflection leaves the plane for heights 1 to 8 and stays inside from 9 rows up; `integer_adm_hip` refuses frames below 17x17 (`adm_frame_size_check()`), so no accepted frame reads outside today. The scale-1 to 3 vertical kernels read per output row and never go out of bounds from 2 rows up. The row's "scale-2 and scale-3 inputs" do not reach `adm_dwt2_load_column()`, which serves scale 0 only.
- `T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`: `integer_vif_hip.c` has no size guard. Its `mirror2_i()` clamps after the reflection, so it does not fault, but below 16 pixels it reads other samples than the CPU, and below 8 pixels scale 3 is empty.

The HIP motion kernel shares the tile shape: on a 17-sample plane its last block reflects halo index 33 to -1 and reads one element before the buffer (ADR-1377 moves `motion_hip` onto that kernel).

Constraints: no score may change for any frame the twins accept (a guard must be the identity where the reflection already stays inside); model dispatch should keep working below a twin's minimum ([ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md)); host-testable without a device; mirror the CUDA twin, which gets the same guards on `fix/cuda-rc3-parity` (ADR-1374 there).

## Decision

1. **One index guard for HIP tile loads.** `core/src/feature/hip/hip_tile_index.h` (plain C and HIP) provides `vmaf_hip_reflect_101()` and `vmaf_hip_tile_index()`, which clamps an already-reflected index into the plane: the identity for every index already inside, so it cannot change a consumed sample. The motion SAD kernel and the `float_motion` kernel pass both tile axes through it; `float_motion_score.hip`'s `fm_mirror()` reflected once and, for the padding threads, read before its input plane at extents 3 to 9 and 17 (index range [-13, 2] at 3, [-1, 16] at 17; host replay in review, `T-HIP-FLOAT-MOTION-TILE-OOB-2026-09-30`).
2. **The ADM scale-0 vertical DWT reads rows through `adm_dwt2_source_row()`.** `core/src/feature/hip/integer_adm/adm_dwt2_rows.h` (plain C and HIP) holds the launch geometry (`ADM_DWT2_*`), the thread's first output row, the kernel's original reflection (`adm_dwt2_reflect_row()`) and the clamped row the kernel now loads. `adm_dwt2.hip` and the host launch in `integer_adm_hip.c` take their geometry from it, and the kernel `static_assert`s the instantiation matches.
3. **vif_hip declares its 16-pixel minimum.** `vif_hip_min_dim()` derives the bound from the filter widths (`(half + 1) << scale` for the scale filters {17, 9, 5, 3} and the decimation filters {9, 5, 3}); `check_context_hip()` returns `-ENOTSUP` below it with `context_fallback_name = "vif"`, so model dispatch computes those frames with the CPU `vif` (bit-identical), and `init()` refuses a direct `vif_hip` request below it with `-EINVAL` before any device work, as `vif_sycl` does. A scaffold build (`enable_hipcc=false`) returns `-ENOSYS` first, for every size (ADR-1264). A scaffold build (`enable_hipcc=false`) returns `-ENOSYS` first, for every size (ADR-1264).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clamp after one reflection, geometry in a host-testable header, VIF CPU fallback (chosen) | Provably in bounds at every size; identity for every accepted frame; replayed on the host for every height; model runs on tiny frames keep working | One more compare per tile load | — |
| Leave the ADM kernel alone because 17x17 is the minimum | No change | The kernel's safety then depends on a check in another file; a future lower minimum or tile change faults silently; the row stays unanswered | The guard costs nothing and the replay proves the claim |
| Reflect repeatedly until inside the plane | Mathematically a valid mirror for any size | Changes which samples padding threads load for no consumer; unbounded-looking loop (HISS-02) | The loaded value is never consumed; a clamp is enough |
| Early-return padding threads before the load | Saves loads | The scale-0 kernel's shared-tile barrier needs every thread; restructuring risks the ADM bit-exactness the twin has | Larger change for no score benefit |
| VIF: rely on `mirror2_i()`'s clamp and accept the difference | No change | Scores differ from the CPU below 16 pixels; scale 3 is empty below 8 | The CPU `vif` is the reference and ADR-1324 routes to it for free |
| VIF: reflect twice (true two-bounce mirror) | Would run tiny frames on the device | Still not the CPU's arithmetic below the bound (the CPU pads differently at those sizes); needs device validation of every scale | vif_sycl chose the fallback for the same reason |

## Consequences

- **Positive**: every HIP tile load in the motion SAD and float-motion kernels and every scale-0 ADM row load stays inside the picture whatever `init()` admits; no score of an accepted frame changes (`test_hip_adm_dwt2_rows` replays every height to 8192 and every motion extent to 1024). Model dispatch computes VIF of frames below 16 pixels on the CPU instead of scoring them differently.
- **Negative**: a direct `--feature vif_hip` request below 16x16 now fails init with `-EINVAL` instead of producing a (wrong) score. Not run on AMD hardware in this change; `test_hip_vif_min_dim` and `test_hip_adm_tiny_frames` carry the device checks (they skip without a device).
- **Neutral / follow-ups**: the CUDA and Metal halves of the two rows stay with their backends. Guarded by `test_hip_adm_dwt2_rows` (device-free, fast suite), `test_hip_vif_min_dim` (declaration and direct init need no device) and the guard cases of `test_hip_kernel_source_contract.py`.

## References

- req: RC3 port brief (2026-09-30): "ADM tiny-height, VIF minimum size"; the rows' own fix instructions ("Fix like the SYCL twin once verified: an ADR-1324 `context_check` that sends frames below 16 pixels to the CPU `vif` under model dispatch, and an `init()` guard for direct requests").
- [Research-2123](../research/2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md) — the SYCL faults; `sycl_tile_index.h`.
- [Research-1377](../research/1377-hip-rc3-cpu-parity.md) — the host replays of the HIP launch grids.
- [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md), [ADR-1103](1103-hip-vif-mirror2-boundary.md), [ADR-0539](0539-hip-adm-kernels-real.md).
