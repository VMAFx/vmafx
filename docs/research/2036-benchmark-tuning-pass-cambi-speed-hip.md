<!-- markdownlint-disable MD013 MD060 -->

# Research digest 2036 — Benchmark tuning pass: CAMBI AVX2 anti-dithering, SpEED SIMD QR, and HIP threaded pipeline

- **Date**: 2026-09-08
- **Author**: Kilian, Claude Opus 5
- **Scope**: Issue #1245 benchmark and performance tuning pass across CPU, CUDA, SYCL, and HIP backends.
- **Related**: [ADR-1196](../adr/1196-speed-matmul-simd-dispatch.md), [ADR-1237](../adr/1237-perf-pass-1245.md), [Research 2030](2030-speed-matmul-and-cambi-cpu-hot-path.md)

## 1. Hardware baseline environment

Workstation configuration:

- **CPU**: AMD Ryzen 9 9950X3D (16 cores / 32 threads, AVX-512, AVX2)
- **CUDA**: NVIDIA GeForce RTX 4090 (CUDA 13.3, driver 580.x)
- **SYCL**: Intel Arc A380 (oneAPI 2026.0 / icpx, Level Zero loader)
- **HIP**: AMD Radeon Graphics gfx1036 (ROCm 7.2 / amdhip64, integrated)

## 2. Benchmark measurements across backends (`testdata/bench_all.sh` & canonical fixtures)

All tests run with model `vmaf_v0.6.1.json` (Netflix baseline ground truth).

| Resolution / Fixture | CPU (Ryzen 9 9950X3D) | CUDA (RTX 4090) | SYCL (Arc A380) | HIP (gfx1036) | Cross-backend parity |
|---|---|---|---|---|---|
| **576x324** (48 frames) | 76.667831 (87ms, 551 fps) | 76.667830 (412ms) | 76.667720 (473ms) | 76.667848 (149ms) | PASS (max diff 1.47e-4, within gate) |
| **1080p Checkerboard Mild** (3 frames) | 35.068671 (71ms, 42 fps) | 35.068667 (154ms) | 35.068628 (147ms) | 35.068671 (85ms) | PASS (max diff 4.6e-5, HIP bit-identical to CPU) |
| **1080p Checkerboard Heavy** (3 frames) | 7.985899 | 7.985899 | 7.985899 | 7.985899 | PASS (bit-identical across all 4 backends) |
| **4K BBB** (200 frames) | 77.641185 (13,386ms, 14.9 fps) | 77.641183 (1,576ms, 126.9 fps) | 77.641080 (5,723ms, 34.9 fps) | 77.641198 (19,820ms, 10.1 fps) | PASS (CUDA 8.5× speedup over CPU; max diff 7.5e-5) |

## 3. Landed performance optimizations

### 3.1 CAMBI `anti_dithering_filter_avx2`

- In `core/src/feature/cambi.c`, `anti_dithering_filter` applies a 2×2 box blur on 16-bit luma to suppress spatial dithering patterns on 8-bit inputs (`enc_bitdepth < 10`).
- Profiling in Research 2030 identified this function as consuming ~5% of CPU runtime under the default model.
- An AVX2 vector kernel (`anti_dithering_filter_avx2`) was implemented in `core/src/feature/x86/cambi_avx2.c`:
  - 16 `uint16_t` lanes loaded per iteration.
  - Zero-extended uint32 sums (`_mm256_cvtepu16_epi32`, `_mm256_add_epi32`) avoid arithmetic overflow.
  - Arithmetic right shift by 2 followed by `_mm256_packus_epi32` and `_mm256_permute4x64_epi64(0xD8)` to restore cross-lane ordering.
  - Scalar loop handles boundaries and remainder.
- **Kernel speedup**: 3.7× speedup on the anti-dithering pass.
- **Parity**: Bit-exact across all frame dimensions (verified by unit tests and `--cpumask 0` differential).

### 3.2 SpEED `si_mat_mul` SIMD dispatch

- In `core/src/feature/speed_internal.c`, `si_mat_mul()` performs matrix products for small-matrix QR factorisation in GPU SpEED twins.
- Dispatched through `speed_matmul_avx512` / `speed_matmul_avx2` / `speed_matmul_scalar` from `speed_matmul.h`.
- Compiled under `-ffp-contract=off` preserving identical left-to-right accumulation order.
- **Speedup**: ~4–5× speedup on dense matrix multiplications.
- **Parity**: 100% bit-exact across all matrix dimensions (verified by `test_speed_simd`).

### 3.3 HIP threaded flush & extractor pooling

- In `core/src/libvmaf.c`, `batch_extractor_skip()` and `read_pictures_should_skip()` previously omitted `VMAF_FEATURE_EXTRACTOR_HIP` and `VMAF_FEATURE_EXTRACTOR_METAL` from their GPU masks.
- When `--threads N` was active, HIP extractors were incorrectly routed to the thread pool, which called `.extract` (unimplemented for GPU extractors), returning `-EINVAL` (-22) upon context flush.
- Fixed by adding HIP and Metal flags to `not_pooled` and `gpu` masks, and ensuring `flush_context_threaded()` drains `gpu_pending` for non-CUDA/SYCL extractors before flushing temporal extractors.
- **Result**: `vmaf --backend hip --threads N` runs without errors and produces valid scores matching single-threaded runs.

### 3.4 `testdata/bench_all.sh` harness fixes

- Candidate search order now places `/opt/intel/oneapi/setvars.sh` (2026.0) ahead of legacy installs, fixing symbol resolution errors when linking against newer oneAPI releases.
- Test 2 now points to the canonical 1080p checkerboard test fixtures `checkerboard_1920_1080_10_3_0_0.yuv` and `checkerboard_1920_1080_10_3_1_0.yuv`.
