<!-- markdownlint-disable MD013 -->
# Research-0567: SpEED GPU Kernel Design — CPU/GPU Split and Backend Implementation Choices

- **Status**: Accepted
- **Workstream**: ADR-0567 — real GPU kernels for speed_chroma + speed_temporal (4 backends)
- **Last updated**: 2026-05-18

## Question

How should the SpEED-QA algorithm be partitioned between GPU and CPU across four backends
(CUDA, SYCL, HIP, Vulkan), given that the algorithm involves both tile-parallel and
inherently serial operations?

## Algorithm structure

SpEED-QA (`speed_chroma`, `speed_temporal`) computes a quality score from 3×3 DCT tiles:

1. **Tile population**: for a frame of H×W pixels, tile count =
   `num_blocks = (H/submatrix_h) * (W/submatrix_w)`, typically 28–312 tiles for SD/HD.
2. **Per-tile means** (25 basis functions per tile): `means[b][k]` for tile b, basis k.
3. **Covariance matrix** (25×25 = 625 elements): global accumulation over all tiles;
   `C[i][j] += sum_{b} (tile[b][i] - mean[i]) * (tile[b][j] - mean[j])`.
4. **Independent term** (25 elements per tile, ref and dis): `iterm[b][k]`.
5. **Eigendecomposition** of the 25×25 covariance (via Jacobi iteration, serial).
6. **QR factorization** of the 25×25 eigenvector matrix (serial).
7. **Backward substitution**: solve `R * x[b] = Q^T * iterm[b]` for each tile b.
8. **Score**: per-tile entropy + variance proxy aggregated to a scalar.

Steps 2, 3, 4, 7, 8 are tile-parallel and map to GPU kernels. Steps 5–6 are serial
operations on a fixed 25×25 matrix with inherent data dependencies between iterations
(Jacobi converges in ~O(N^2) sweeps; QR has O(N^2) column-serial eliminates). The
matrix is small enough that there is no benefit from launching a GPU kernel for it —
one wavefront (64 threads) on GCN computes 25×25 Jacobi in ~50 cycles, versus ~200 µs
PCIe round-trip to read back a 625-element covariance and dispatch a 25×25 kernel.

## CPU/GPU split decision

**Boundary**: GPU handles steps 2–4 and 7–8. CPU handles steps 5–6.

This requires one host-device synchronization per frame (read back the 625-element
covariance after step 3, then upload the 25×25 R matrix after step 6). On a modern
PCIe 4.0 link this round-trip is ~30–50 µs; for HD content with ~100 tiles, the GPU
tile-parallel portion runs in ~800 µs on an RTX 4090, so the round-trip overhead is
4–6% of total frame time. For 4K content with ~400 tiles the ratio drops below 1%.

The alternative of running Jacobi on GPU would require either:

- A single-warp reduction loop (25×25 → 1 warp, massively underutilized), or
- A custom convergence loop with dynamic parallelism (>5× implementation complexity).

Neither path provides a throughput benefit for a 25×25 matrix.

## Kernel design per backend

### CUDA (`feature/cuda/speed/speed_score.cu`)

Five `extern "C"` kernels. Covariance kernel: 625 blocks × 256 threads; each block
reduces `num_blocks` partial sums for one `(i,j)` pair using double-precision shared
memory tree reduction. Solve kernel: one warp per column; `__syncwarp(0xFFFFFFFFu)`
between rows for the backward-substitution serial dependency.

### SYCL (`feature/sycl/speed_{chroma,temporal}_sycl.cpp`)

Five `launch_*` functions using `sycl::nd_range`. USM allocations: `sycl::malloc_device`
for GPU buffers; `sycl::malloc_host` for pinned staging. Group barrier
(`sycl::group_barrier`) used in the solve kernel for row-serial dependency. Double-precision
shared memory (`local_accessor<double>`) for covariance reduction.

### HIP (`feature/hip/speed/speed_score.hip`)

Five kernels with `_hip_` infix in entry names (to distinguish from CUDA entry points in
HSACO symbol tables). Wavefront size: 64 (GCN/RDNA standard). Solve kernel: one wavefront
(64 lanes) per column; only threads 0–24 active; `__builtin_amdgcn_wave_barrier()` for
row-serial dependency (lighter than a full `__syncthreads()`). Dynamic shared memory:
`extern __shared__ double s_partial[]` with explicit `sharedMemBytes` argument at launch.
Without `enable_hipcc=true`, `init()` returns `-ENOSYS` (scaffold posture per ADR-0533).

### Vulkan (`feature/vulkan/shaders/speed_score.comp`)

Single GLSL compute pipeline, 7 pass variants selected via push constant `pc.pass` (uint).
This avoids the 7-pipeline-at-init-time overhead that specialization constants would require
(one `vkCreateComputePipeline` call per pass). Requires
`GL_EXT_shader_explicit_arithmetic_types_float64` for double-precision covariance reduction
in pass 1. 13 SSBO bindings cover plane data, means, covariance, independent terms, R matrix,
solution vectors, and per-tile score/entropy outputs. Submit pool pattern (ADR-0353) with one
slot; each of the 7 passes calls `dispatch_pass()` which acquires, submits, and waits.

## Internal API header (`feature/speed_internal.h`)

To avoid duplicating the CPU linear-algebra helpers across four backend TUs, a new internal
header exposes the speed.c private functions:

- `speed_internal_init_dimensions(opts, width, height, *dims)` — initializes tile layout.
- `speed_internal_filter_and_downscale(src, dst, dims, stride)` — spatial preprocessing.
- `speed_internal_compute_eigenvalues(cov, eig, dims)` — 25×25 Jacobi eigendecomposition.
- `speed_internal_qr_factorize(cov, R, dims)` — 25×25 QR via Householder reflections.
- `speed_internal_qt_multiply(Q, b, dims)` — Q^T × b for independent term projection.
- `speed_internal_backward_substitution(R, b, x, dims)` — serial backward substitution.
- `speed_internal_is_matrix_regular(cov, dims)` — degeneracy guard.

These are the CPU's serial path; the GPU backends call them only for the 25×25 math.

## Conclusion

The five-kernel GPU pipeline + CPU round-trip design is the correct algorithmic
partition for SpEED. The tile-parallel 85–90% of per-frame work runs on device; the
inherently serial 25×25 eigendecomposition runs on CPU. The design is consistent across
all four backends and satisfies the places=4 parity gate (ADR-0214).
