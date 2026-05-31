# HIP batch-1: `integer_psnr_hip` and `float_ansnr_hip` real kernels (ADR-0372)

Promotes two HIP feature extractors from `-ENOSYS` scaffolds to real
HIP Module API consumers, following the pattern established by PR #612
(`float_psnr_hip`, ADR-0254).

### What ships

- **`psnr_score.hip`** — device kernel for `integer_psnr_hip`:
  uint64 atomic-SSE reduction, warp-size-64 `__shfl_down` (split into
  two uint32 shuffles), 8-bpc and 16-bpc entry points.
- **`float_ansnr_score.hip`** — device kernel for `float_ansnr_hip`:
  per-block (sig, noise) float partial pairs, 3×3 ref Gaussian +
  5×5 dis filter with shared-memory mirror-padded tile, 8-bpc and
  16-bpc entry points.
- **Host TUs** (`integer_psnr_hip.c`, `float_ansnr_hip.c`): promoted
  from scaffold to real `hipModuleLoadData` + `hipModuleGetFunction` +
  `hipModuleLaunchKernel` paths under `#ifdef HAVE_HIPCC`. Without
  `HAVE_HIPCC`, the scaffold `-ENOSYS` contract is preserved.
- **`kernel_template.{h,c}`**: adds `vmaf_hip_kernel_submit_post_record`
  (also being added by PR #612; on merge conflict keep one copy).
- **`meson.build`**: adds `hip_hsaco_sources` HSACO build pipeline
  (`hipcc --genco` + `xxd -i`), mirroring the CUDA `ptx_arrays` pattern.
  Controlled by the existing `enable_hipcc` option.

### How to use

```bash
# Build with real HIP kernels (requires ROCm 6+ and hipcc in PATH):
meson setup build -Denable_hip=true -Denable_hipcc=true
ninja -C build

# Run luma PSNR on AMD GPU:
vmaf --reference ref.yuv --distorted dis.yuv --feature psnr_hip \
     --width 1920 --height 1080 --pixel_format 420

# Run float ANSNR on AMD GPU:
vmaf --reference ref.yuv --distorted dis.yuv --feature float_ansnr_hip \
     --width 1920 --height 1080 --pixel_format 420
```

### Skipped (batch-2 candidates)

- `float_ssim_hip`: two-pass design with five intermediate device
  buffers and three kernel functions — non-trivial ABI adaptation.
- `motion_hip`: different API shape (raw `vmaf_hip_motion_*` functions,
  not a `VmafFeatureExtractor`) — requires complete API rewrite.
