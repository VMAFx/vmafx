<!-- markdownlint-disable MD013 MD060 -->
# ADR-1378: Run the HIP CAMBI extractor entirely on the device

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: hip, gpu, cambi, performance, numerics, rc3, fork-local

## Context

`cambi_hip` followed the Strategy II hybrid of [ADR-0205](0205-cambi-gpu-feasibility.md): `submit()` ran `vmaf_cambi_preprocessing` on the host and uploaded the result, the device ran the spatial mask, decimation and mode filter, and for each of the five scales `cambi_hip_readback_scale` copied image and mask back and waited on the stream (`hipMemcpy2DAsync` + `hipStreamSynchronize`) so the host could run `vmaf_cambi_calculate_c_values` and `vmaf_cambi_spatial_pooling`. That is five device-to-host round trips and host waits per frame, with the GPU idle while the host works (`T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29`). The twin also lacked `cambi.c`'s init guard against windows whose square reaches the 4226-entry reciprocal table, so a window above 65 x 65 would have reached the host `c_value_pixel()` and read past the table.

[ADR-1357](1357-sycl-cambi-device-resident.md) solved the same problem for SYCL and was verified bit-exact on an Arc B580 and a UHD 770. The maintainer's RC3 brief asks for the same on HIP: "there shouldnt be any gpu cpu rountrips", one wait per frame at collect, CPU-exact results, and one implementation per behaviour (HISS-19). A sibling port of the same design to CUDA runs in parallel.

## Decision

1. **Every CAMBI stage on the device, the ADR-1357 design.** Per frame `submit()` enqueues on the extractor's stream: one staged upload of the distorted luma (`vmaf_hip_picture_upload_staged()`, [ADR-1377](1377-hip-motion-diff-first.md)), a reset of the per-frame state, input validation, preprocessing (10-bit conversion, resize through init-time index tables, anti-dither), the tiled 7x7 spatial mask, and per scale decimation, the horizontal and vertical mode filter (the vertical pass writes the level map), the run / change bit masks, the column-owned sliding-histogram c-values with radix pass 0, the radix select passes 1 and 2 and the exact 128-bit top-K sum; then one 88-byte `CambiHipResults` copy to pinned memory. `collect()` is the only wait: it converts the five sums with `cambi.c`'s `vmaf_cambi_fixed_topk_mean()` and weights them with `vmaf_cambi_weight_scores_per_scale()`.
2. **One header for the kernels and the replay.** Every per-work-item routine is in `core/src/feature/hip/integer_cambi/cambi_hip_device.h`, compiled by hipcc for `cambi_score.hip` and by the host compiler for `core/test/test_hip_cambi_device_math.c`. The parameter block (every buffer pointer and per-scale dimension) lives in device memory, written once at init by `cambi_hip_plan()` / `cambi_hip_plan_bind_scales()` (`integer_cambi_hip.h`), which the replay calls too, so the replay runs with the block the extractor uploads.
3. **`cambi.c`'s helpers, not copies.** The adjusted window, the mask index, the resize index tables, the contrast weights, the reciprocal table, the top-K mean and the window guard come from `cambi_internal.h` (`vmaf_cambi_adjust_window`, `vmaf_cambi_mask_index`, `vmaf_cambi_resize_source_indices`, `vmaf_cambi_contrast_weights`, `vmaf_cambi_reciprocal_lut`, `vmaf_cambi_fixed_topk_mean`, `vmaf_cambi_check_window_fits_lut`); `cambi.c`'s own init now calls the guard through the same function. The CUDA port adds the same helpers with the same signatures; whichever lands second keeps one copy.
4. **The init guard before any device work.** `init()` rejects a window whose adjusted size, at the encode or the source resolution after the high-res speed-up, has `window^2 >= 4226`, with -EINVAL and cambi.c's message, before it touches the device, so the check needs no AMD device. A build without hipcc reports -ENOSYS before any check, as [ADR-1264](1264-hip-scaffold-enosys-contract.md) requires of every HIP scaffold path.
5. **No fp64, integer reductions.** Every reduction is over integers (order-independent on wave32 and wave64), and the only float operations are c-value products compared or scaled by 2^24, which cannot contract. The numerical contract is ADR-1357's: bit-identical to the CPU whenever `cambi.c`'s own double top-K sum is exact.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port ADR-1357 through a shared per-work-item header (chosen) | CPU-exact by construction; the same header is replayed on the host in CI, so the numerics are pinned without an AMD device; one readback and one wait per frame | About 64 launches per frame on one stream; the per-chunk column histograms (bounded at 64 MiB) | — |
| Keep the hybrid and batch the five readbacks | Smallest change | Still a host residual and a round trip per frame; the maintainer asked for none | Brief |
| Port the SYCL kernels without a host replay | Less code | Nothing checks the arithmetic until someone runs an AMD device | No AMD device in CI; the replay found no difference only because it can |
| Keep the twin's own copies of the window, mask-index and resize helpers | No change to `cambi.c` | A third copy of each (SYCL, CUDA, HIP) that can drift from the CPU | HISS-19 |
| Replay the kernels on the host with one work-item at a time | Simplest replay | Misses races between concurrent work-items | The replay runs every c-values work-item in lockstep with a canary behind the last histogram |

## Consequences

- **Positive**: no host stage and one wait per frame; scores are expected to equal `--backend cpu` bit for bit wherever the CPU's double sum is exact (every sub-4K frame, 47 of 50 BBB 4K frames on SYCL). `cambi_hip` now refuses the same oversized windows as `cambi.c`. The replay (`test_hip_cambi_device_math`, `fast` suite) compares every scale's c-value plane and score with `cambi.c` over 12 fixtures (8 to 12 bits, resize, anti-dither, the high-res speed-up, windows, contrast depths, row-chunk layouts) and kills every planted regression in `mutate` runs of the device header and the plan.
- **Negative**: not run on an AMD device in this change: the verify and timing commands are in the `T-HIP-CAMBI-HOST-RESIDUAL-2026-09-29` row of `docs/state.md`. Device memory grows by the column histograms and the float c-value plane.
- **Neutral / follow-ups**: the SYCL twin keeps its own copies of the helpers until the CUDA port moves it onto them; `test_hip_device_resident_contract.py` pins the design at the source level (no host stage, staged upload, one readback, the wait in `collect()`, fp64-free kernels).

## References

- req: RC3 port brief (2026-09-30): "there shouldnt be any gpu cpu rountrips"; device-resident cambi "matching the CPU (... cambi exact top-K)", one wait per frame at collect.
- [ADR-1357](1357-sycl-cambi-device-resident.md) — the SYCL design ported here; [Research-2122](../research/2122-sycl-cambi-device-resident.md).
- [Research-1378](../research/1378-hip-cambi-speed-device-resident.md) — the HIP port, the host replays and what they establish without a device.
- [ADR-0205](0205-cambi-gpu-feasibility.md), [ADR-1219](1219-gpu-cambi-tvi-shared-bisection.md), [ADR-1377](1377-hip-motion-diff-first.md), [ADR-0214](0214-gpu-parity-ci-gate.md).
