<!-- markdownlint-disable MD013 MD060 -->
# ADR-1374: CUDA integer ADM and VIF guard tiny frames like their SYCL twins

- **Status**: Accepted
- **Date**: 2026-09-30
- **Deciders**: lusoris
- **Tags**: cuda, gpu-parity, adm, vif, fork-local

## Context

Two SYCL defects found on 2026-09-29 have CUDA twins that no NVIDIA device has checked.

`T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29`: the SYCL integer ADM vertical DWT loaded a fixed tile with one reflection and read before its buffer on planes of 8 rows or fewer (`T-SYCL-TILE-HALO-OOB-READ-2026-09-29`). The CUDA `adm_dwt2_load_column()` reflects the bottom edge once with `y_in - max(0, 2 * (y_in - h) + 1)`, which is negative once `y_in >= 2h`, and loads for every thread of the last block row, including the ones whose outputs lie past the plane. Whether any thread of the launched grid reaches such a row was never traced to a size.

`T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29`: every integer VIF scale reflects its filter taps once, which stays inside the plane only while floor(dim / 2^s) exceeds the tap half-width, 16 pixels for the {17, 9, 5, 3} filters. `vif_sycl` now declares that bound through the [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) first-picture gate (maintainer decision, 2026-09-29: fall back to the CPU). `vif_cuda` had no size guard. Its `filter1d.cu` loaders clamp the taps a second reflection would need, which keeps the loads inside the buffers but is not the CPU's value.

Constraints: no device was available to run either case; a guard must not change any score the twin already computes correctly; one behaviour, one implementation (HISS-19).

## Decision

1. **ADM: the row and tap arithmetic moves into a header the host can test.** `core/src/feature/cuda/integer_adm/adm_dwt2_rows.h` holds the scale-0 launch geometry, the source row of every thread (`adm_dwt2_reflect_row()`) and the scale 1-3 tap index (`adm_dwt2_s123_tap()`); the kernels and their launches use it, and `static_assert`s tie the kernel instantiation to the header's geometry. The scale-0 load clamps the reflected row into the plane (`adm_dwt2_source_row()`, through `cuda_tile_index.h`), the identity for every row a valid output consumes. `test_cuda_adm_dwt2_rows` replays every thread row of the launched grid for every height up to 8192, device-free: from the 17-row ADM minimum up the single reflection never leaves the plane, so the clamp cannot change a score; below 9 rows it would, and the clamp keeps the kernel in bounds whatever `init()` admits; the scale 1-3 taps stay inside every plane the ladder produces.
2. **VIF: the ADR-1324 gate, as on SYCL.** `vif_cuda` derives its minimum from its own filter widths (`vif_cuda_min_dim()`, 16), declares `context_check` (`-ENOTSUP` below it) and `context_fallback_name = "vif"`, so model dispatch and the CLI twin selection compute smaller frames with the CPU `vif`; a direct `vif_cuda` request below it fails `init()` with `-EINVAL` before it touches the CUDA state.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Header-held arithmetic, host replay of the launched grid, clamp (chosen) | Proves the bound for every height without a device; the kernel keeps in bounds on its own; zero score change | Touches upstream-mirror NVIDIA code (`adm_dwt2.cu`) | — |
| Declare the ADM row not affected from a hand trace | No code change | A trace nobody can re-run; the next geometry change silently reopens the question | The test makes the trace executable |
| Raise the ADM minimum instead of clamping | Keeps the kernel byte-identical | The minimum is the CPU's (17x17); raising it drops sizes the CPU scores | CPU parity |
| VIF: fix the tile loaders to reflect twice | Scores below 16 on the device | Needs the CPU's small-plane behaviour reproduced exactly and verified on hardware; SYCL chose the fallback | Maintainer decision for SYCL (2026-09-29) applies to the twin |
| VIF: silently compute below 16 on the device | No behaviour change | Scores differ from the CPU without any signal | CPU parity |

## Consequences

- **Positive**: every row and tap the CUDA ADM DWT kernels load is shown inside the plane for every height, device-free, and the scale-0 load stays in bounds even below the ADM minimum. VIF frames below 16 pixels get the CPU's scores under model dispatch, bit for bit, and a direct request fails loudly instead of returning a clamped score.
- **Negative**: `vif_cuda` refuses frames below 16x16 that it used to score (with clamped taps); a caller naming `vif_cuda` for such frames must use `vif`.
- **Neutral / follow-ups**: verified on an RTX 4090 on 2026-09-30: `compute-sanitizer --tool memcheck` finds 0 errors in `test_cuda_adm_tiny_frames` and `test_cuda_vif_min_dim`, the `adm` parity gate reads 1.0e-6 on this change and on `master` alike, and below 16 pixels `vif` equals the CPU (the CPU computes it). The HIP halves of both rows and the Metal VIF twin stay open. Guarded by `test_cuda_adm_dwt2_rows` (fast suite, every host), `test_cuda_vif_min_dim` (declaration and direct rejection device-free; model boundary with a device), `test_cuda_adm_tiny_frames` under `compute-sanitizer` on a device, and the guard cases of `test_cuda_kernel_source_contract.py`.

## References

- req: RC3 CUDA port brief (2026-09-30): "ADM tiny-height, VIF minimum size ... T-CUDA-HIP-ADM-DWT-VERT-TINY-HEIGHT-OOB-2026-09-29 (CUDA half); T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29 (CUDA half)".
- [Research-1372](../research/1372-cuda-rc3-parity-port.md) — launch-grid traces.
- [Research-2123](../research/2123-sycl-b580-psnr-hvs-and-tile-halo-faults.md) — the SYCL faults; [ADR-1324](1324-gpu-float-ssim-auto-scale-fallback.md) — the first-picture gate.
