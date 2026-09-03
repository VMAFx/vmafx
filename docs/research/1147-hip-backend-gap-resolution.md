<!-- markdownlint-disable MD013 -->
# Research-1147: HIP Backend Gap Resolution and AMD iGPU Parity

- **Status**: Active
- **Workstream**: ADR-1154
- **Last updated**: 2026-09-03

## Question

Why did 13 of 19 registered HIP feature extractors leave `VMAF_FEATURE_EXTRACTOR_HIP` cleared (`.flags = 0`), silently falling back to CPU execution in VMAF models, what defects existed in their kernel argument packaging and memory copying paths, and what actually executes on AMD Granite Ridge / Raphael-class integrated GPUs (`gfx1036`)?

## Sources

- `core/src/feature/hip/` and `core/src/feature/hip/*/*.hip` (HIP host glue and device kernels)
- `core/src/feature/cuda/` (CUDA reference twins)
- AMD ROCm HIP Runtime API documentation (`hipModuleLaunchKernel`, `hipMemcpy2DAsync`)
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) (GPU parity CI gate)
- [ADR-0530](../adr/0530-hip-feature-flag-promotion-and-picture-buffer.md) (HIP feature flag promotion and picture buffer type)
- [ADR-0533](../adr/0533-hip-all-extractors-registration-sweep.md) (HIP extractor registration sweep)
- [ADR-0564](../adr/0564-integer-ssim-gpu-real-kernels.md) (Integer SSIM GPU real kernels)
- [ADR-1103](../adr/1103-hip-vif-mirror2-boundary.md) (HIP VIF mirror2 boundary fix)
- [ADR-1154](../adr/1154-hip-backend-gaps.md) (HIP backend gap closure)

## Findings

### 1. `hipModuleLaunchKernel` Argument Packaging Conventions

The HIP Driver/Module API requires that each entry in `void *args[]` points to the storage of the kernel argument:

- For a scalar `unsigned width`, `args[i] = &width`.
- For a device pointer `void *d_buf`, `args[i] = &d_buf` (the address of the pointer variable).

Several HIP host extractors contained argument packaging bugs that caused immediate GPU virtual memory access faults:

- **`float_psnr_hip.c`**: Passed `s->rb.device` directly instead of a pointer to a pointer variable (`&partials_dev`). The kernel dereferenced the partials buffer pointer as a memory address, faulting on NULL.
- **`float_moment_hip.c`**: The parameter pack in `moment_hip_launch` transposed arguments: `&s->ref_in, &row_w, &s->dis_in, &row_w...` instead of `ref, dis, ref_stride, dis_stride...`. The kernel received the stride value (`576` = `0x240`) in place of the distorted picture pointer, faulting at address `0x240`. In addition, `s->rb.device` was passed directly instead of `&sums_dev`.
- **`integer_ms_ssim_hip.c`**: The kernel `ms_ssim_vert_lcs` in `ms_ssim_score.hip` writes `double` partials and expects `double c1, c2, c3`, but the host state declared them as `float` and allocated partials as `sizeof(float)` elements, causing memory corruption and invalid argument values. Correcting `c1..c3` and partials buffers to `double` restored correct execution.

Correcting these pointer-to-pointer references and type widths restored clean kernel execution.

### 2. Option Serialization and Feature Dictionary Timing

In `integer_cambi_hip.c`, `vmaf_feature_name_dict_from_provided_features` was called after overwriting state fields (`s->full_w = w`, `s->full_h = h`). Because default options for width and height were set to 0 in the option table, `vmaf_feature_name_dict_from_provided_features` treated the non-zero resolution values as user overrides and serialized them into the feature dictionary as `cambi_full_w_576_full_h_324`. Lookups for canonical `"cambi"` subsequently failed. Moving the dictionary creation before internal dimension assignment resolved the issue.

### 3. Chroma Plane Copying

In `integer_psnr_hip.c`, `submit_fex_hip` only copied the luma plane (plane 0). When `enable_chroma=true`, the kernel launched across all three planes, but planes 1 and 2 contained uninitialized device memory. Looping over `s->n_planes` in `submit_fex_hip` fixed chroma PSNR parity.

### 4. SSIM Algorithm Divergence on HIP

`integer_ssim_score.hip` was originally ported from `core/src/feature/cuda/integer_ssim/ssim_score.cu` (which is actually the `float_ssim` 11-tap Gaussian kernel), rather than `core/src/feature/cuda/integer_ssim/integer_ssim_score.cu` (the true 9-tap separable integer SSIM kernel with int64 moments).

Running `integer_ssim_hip` on GPU produces a delta of `4.53e-3` compared to CPU integer SSIM. Per ADR-0564, the purpose of `integer_ssim` is strict reproducibility with the CPU reference; silent numeric drift on a GPU backend is unacceptable. Therefore, `integer_ssim_hip.c` must keep `.flags = 0` (falling back to CPU) until the 9-tap int64 kernel (`integer_ssim_score.cu`, ~280 LOC) is ported to HIP.

### 5. Picture Staging in Integer ADM vs Float ADM

Unlike CUDA which supports device picture pools (`vmaf_cuda_picture_alloc`), HIP pictures arrive as `VMAF_PICTURE_BUFFER_TYPE_HOST`. Feature extractors must manage their own HtoD staging buffers.

- `float_adm_hip.c` allocates device buffers `s->src_ref` and `s->src_dis` and copies host frames via `hipMemcpy2DAsync`. It executes cleanly on the GPU and passes parity (2/2 tests green).
- `integer_adm_hip.c` directly passed `ref_pic->data[0]` (a host pointer) to `dwt2_8_device_hip`, which launched kernels that faulted on CPU addresses. Integer ADM remains deferred (`.flags = 0`) pending either internal staging buffers (~350 LOC) or the HIP device picture pool (T7-10c, ~600 LOC).

### 6. Hardware Verification on AMD Granite Ridge (`gfx1036`)

10 HIP extractors were successfully promoted to active GPU execution:

1. `integer_cambi_hip` (parity test passed 2/2)
2. `ciede_hip` (parity test passed 2/2)
3. `integer_psnr_hip` (parity test passed 2/2)
4. `float_psnr_hip` (parity test passed 2/2)
5. `float_moment_hip` (parity test passed 3/3, 48-frame run at 318 FPS)
6. `integer_motion_v2_hip` (parity test passed, 48-frame run at 308 FPS)
7. `float_motion_hip` (parity test passed 2/2)
8. `float_ssim_hip` (parity test passed 2/2)
9. `integer_ms_ssim_hip` (parity test passed 2/2)
10. `integer_psnr_hvs_hip` (parity test passed 2/2)
11. `float_adm_hip` (parity test passed 2/2)

Together with the 6 previously active extractors (`integer_motion_hip`, `integer_vif_hip`, `speed_chroma_hip`, `speed_temporal_hip`, `ssimulacra2_hip`, `float_vif_hip`), 17 of 19 registered HIP extractors are now actively dispatching and verified on AMD GPU hardware.

## Alternatives explored

- **Promoting `integer_ssim_hip` with relaxed tolerance**: Rejected per ADR-0564; shipping a float-divergent kernel under the integer SSIM feature name breaks reproducibility with CPU ground truth.
- **Re-implementing HIP device picture pool (T7-10c) immediately**: Evaluated; implementing the full asynchronous GPU picture pool across all frame formats exceeds 800 LOC. Feature extractors with internal staging buffers provide immediate working GPU paths without pool coupling.

## Open questions

- Porting the 9-tap int64 integer SSIM kernel (`integer_ssim_score.cu` ~280 LOC) to `integer_ssim_score.hip` to close the last remaining numeric gap.
- Implementing zero-copy DMA-BUF picture import for HIP via ROCm `hipImportExternalMemory` (T7-10c).

## Related

- ADRs: [ADR-1154](../adr/1154-hip-backend-gaps.md)
