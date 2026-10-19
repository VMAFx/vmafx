---
paths:
  - core/src/feature/speed_internal.c
  - core/src/feature/speed_internal.h
invariant: speed_internal shared helper TU wiring, matrix multiplication dispatch, and CUDA dependencies.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# speed_internal Helper TU and Matrix Multiplication

## `speed_internal.c` is the shared CPU helper TU for the SpEED GPU twins (ADR-0964)

`core/src/feature/speed_internal.{h,c}` is contract between
CPU SpEED extractor (`speed.c`) and GPU twins
(`feature/{cuda,hip,sycl}/speed_{chroma,temporal}_*.{c,cpp}`).
header declares 9 functions (dimensions, float-stride,
filter+downscale, covariance, eigendecomp, QR factorise, Q^T
multiply, backward-substitution, regularity check); GPU TUs
`#include` it and call 7 of them.

**Wiring invariant — any new feature extractor that ships GPU
twins must have FIVE companion changes in same PR**:

1. **CPU implementation TU** — scalar reference under
   `core/src/feature/<name>.c` (or, for shared math,
   `<name>_internal.c` under same directory).
2. **Header declaration** in `core/src/feature/<name>.h` or
   `<name>_internal.h` — GPU-callable surface.
3. **Meson source list** —
   - CPU TU added to appropriate block in `core/src/meson.build`
     (`libvmaf_feature_sources` or under `if float_enabled`).
   - HIP TU added to `core/src/hip/meson.build` `hip_sources +=
     files(...)`.
   - SYCL TU added to `core/src/meson.build` `sycl_feature_sources`.
   - CUDA TU added to `core/src/meson.build`
     `libvmaf_feature_sources` under `if is_cuda_enabled`.
4. **Registry entry** in
   `core/src/feature/feature_extractor.c` — `extern` declaration
   under right `#if HAVE_<BACKEND>` block plus row in
   `feature_extractor_list[]`.
5. **CPU-vs-GPU parity test** under `core/test/`, mirroring
   `test_sycl_motion3_parity.c` (or
   `test_sycl_speed_{chroma,temporal}_parity.c` for SpEED
   example). Must skip cleanly when relevant GPU device is
   not visible.

**Host tail of the GPU twins (ADR-1477).** `speed_internal_gpu_tail_scores()`
forms entropies + score from the block a pipeline reads back
(`SpeedGpuTailLayout` in `speed_gpu_common.h`: status words, 25 eigenvalues,
per-block variances, per channel). Statements = `speed.c`'s fp64 `log2()`
ones, verbatim; called by CUDA / HIP / SYCL pipelines after their one wait.
No kernel evaluates a logarithm. Edit `update_entropy()` / `get_speed_score()`
/ `speed_extract_score()` in `speed.c` -> edit the tail in same PR. Never move
entropy or score back to a device (device log2 != host log2: cells were
`LIBM_TWINS` bounds until ADR-1477, exact since). Allocation-free; caller
owns the `2 x blocks` scratch.

If any of five is missing, symptom is silent:
extractor name does not resolve in `vmaf_get_feature_extractor_by_name()`
and GPU pipeline never runs, while CI stays green because no
parity gate fires.  `feature_extractor_list_audit()` function
in `feature_extractor.c` catches duplicate registrations, not
*missing* ones.

## `matrix_mul` dispatches; `si_mat_mul` deliberately does not (ADR-1196)

`speed.c`'s `matrix_mul()` no longer contains multiply loop. It
takes `speed_matmul_fn` (declared in
[`speed_matmul.h`](../speed_matmul.h)) and forwards to
`speed_matmul_scalar` / `speed_matmul_avx2` / `speed_matmul_avx512`,
chosen in `speed_dispatch_cpu_kernel()` from `vmaf_get_cpu_flags()`.
pointer is threaded explicitly through
`matrix_qr_decomposition()` and `solve_linear_system()`, which upstream
Netflix does not do — expect signature conflict there on next
`/sync-upstream`, and re-thread rather than dropping parameter.

**Two invariants hold that pointer's value:**

1. **kernels must stay bit-identical to scalar reference.**
   argument is that `j` in `dst[i][j] += x[i][k] * y[k][j]` is output
   index, not reduction axis, so vector width cannot reorder any single
   element's accumulation over `k`. only way to break that is FMA
   contraction, which is why
   `x86/speed_matmul_avx2.c` and `x86/speed_matmul_avx512.c` each compile
   in their own `-ffp-contract=off` static library in
   `core/src/meson.build`. **Do not move either file into
   `x86_avx2_sources` / `x86_avx512_sources`** — those libraries are built
   with `-mfma` and contraction on, and `memcmp` cases in
   `core/test/test_speed_simd.c` will start failing.
2. **`speed_matmul_scalar` is intentionally non-`static`.** parity
   test compares twins against production reference itself, not
   against copy. Re-`static`-ing it breaks test link.

`si_mat_mul()` in `speed_internal.c` — ADR-0964 duplicate of same
i-k-j loop, used by host side of GPU SpEED twins — is
dispatched through `speed_matmul_avx512` / `speed_matmul_avx2` / `speed_matmul_scalar`
per ADR-1237 (gated behind same bit-exact contract as `speed.c`'s `matrix_mul()`).

**Source-of-truth note**: `speed_internal.c` duplicates ~600 LOC
of pure math (eigendecomp, QR, matrix helpers) from `speed.c`.
This is deliberate (see ADR-0964 Alternatives) — keeping
`speed.c` clean of `extern` exposures preserves its
Netflix-mirrored status for `/sync-upstream` cadence. If
either copy gets bug-fix, mirror it to other;
CPU-vs-SYCL and CPU-vs-CUDA parity tests will surface drift at CI time.

**CUDA TU dependency on `CudaFunctions` schema (ADR-0965)**: two
CUDA SpEED TUs (`cuda/speed_chroma_cuda.c` and
`cuda/speed_temporal_cuda.c`) call into `CudaFunctions` table
using **two specific members**:

- `cuMemHostAlloc((void **)&ptr, size, flags)` — pinned host
  allocation (NOT `cuMemAllocHost`; that variant is not in table).
- `cuMemFreeHost(ptr)` — pinned host free.

two CUDA TUs also use `CHECK_CUDA_GOTO(cu_f, CALL, label)` for all
fallible CUDA calls (NOT legacy `CHECK_CUDA` macro which was removed).
If `CudaFunctions` table ever gains or renames these members, update
all four `ALLOC_HOST` / `FREE_HOST` macro call sites in both TUs in
same PR. See `core/src/cuda/cuda_helper.cuh` for macro contract and
`core/src/cuda/picture_cuda.c` / `core/src/cuda/common.c` for
canonical usage of these members across codebase.

## The prescaled plane limit is part of the shared geometry

`speed_internal_init_dimensions()` refuses an allocation plane past the `int`
index of `vif_tools.c` (`vif_plane_fits_int_index()`), as `speed.c`'s own
`speed_init_dimensions()` does. Every SpEED device twin takes its geometry
from this function, so they refuse the same option combinations as the CPU
(T-PRESCALED-PLANE-INT-INDEX-2026-10-05).
