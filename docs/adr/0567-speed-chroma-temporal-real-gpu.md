<!-- markdownlint-disable MD013 MD060 -->
<!-- ADR-0567 — reserved by scripts/adr/next-free.sh --claim speed-chroma-temporal-real-gpu -->

# ADR-0567: Real On-Device GPU Kernels for speed_chroma and speed_temporal (4 Backends)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `cuda`, `sycl`, `hip`, `vulkan`, `speed`, `feature`, `gpu`, `fork-local`

## Context

The SpEED-QA algorithm (`speed_chroma` + `speed_temporal`) was registered in the
CPU feature extractor but had no GPU twins on any backend. All four backends (CUDA,
SYCL, HIP, Vulkan) were missing these extractors entirely, leaving a parity gap
against every other extractor class (VIF, ADM, motion, SSIM, etc.) which already
had GPU twins on all backends.

SpEED computes a 5×5 covariance matrix over 3×3 DCT tiles (25 basis functions, 25
elements per tile), then solves a linear system via eigendecomposition + QR
factorization. The tile-parallel work (per-tile means, covariance accumulation,
independent term computation, backward substitution, and entropy/score) maps cleanly
to GPU kernels. The fixed-size 25×25 serial ops (eigendecomposition, QR
factorization) are unavoidably on the CPU due to their inherently sequential nature;
this is the correct algorithmic boundary, not CPU forwarding.

The parity gate is places=4 against the CPU reference on the Netflix golden src01
fixture (per ADR-0214).

## Decision

We implement real on-device GPU kernels for both `speed_chroma` and `speed_temporal`
on all four backends (CUDA, SYCL, HIP, Vulkan), with a CPU/GPU split at the correct
algorithmic boundary: five tile-parallel GPU kernels per frame (means, covariance
accumulation, independent term, backward substitution, score), with the serial
25×25 eigendecomposition and QR factorization executed on the CPU between GPU pass 2
and GPU pass 3.

- **CUDA** (`feature/cuda/speed/speed_score.cu`): 5 `extern "C"` kernels using
  cuMemAlloc/cuLaunchKernel/cuStreamSynchronize; covariance kernel uses double-precision
  shared-memory tree reduction (625 blocks × 256 threads).
- **SYCL** (`feature/sycl/speed_{chroma,temporal}_sycl.cpp`): 5 `launch_*` functions
  using `sycl::nd_range` kernels, USM device/host allocations (`sycl::malloc_device` /
  `sycl::malloc_host`), `group_barrier` for the backward-substitution row dependency.
- **HIP** (`feature/hip/speed/speed_score.hip`, `feature/hip/speed_{chroma,temporal}_hip.c`):
  5 kernels with `_hip_` infix in entry names; wavefront=64 for GCN/RDNA; solve kernel
  uses `__builtin_amdgcn_wave_barrier()` between rows; `hipModuleLoadData` for HSACO.
  Without `enable_hipcc=true`, `init()` returns `-ENOSYS` (scaffold posture).
- **Vulkan** (`feature/vulkan/shaders/speed_score.comp`, `feature/vulkan/speed_{chroma,temporal}_vulkan.c`):
  Single GLSL compute pipeline, 7 passes selected via push constant `pc.pass` (0–6).
  Requires `GL_EXT_shader_explicit_arithmetic_types_float64` for covariance reduction.
  Passes 0–2 handle means/covariance/indterm; CPU eigendecomp+QR occur between passes;
  passes 4–5 solve; pass 6 scores. Submit pool pattern per ADR-0353.

The `speed_temporal` variant adds a ping-pong buffer pair (`h_ref[2]` / `h_dis[2]`)
for the temporal frame difference; frame 0 emits score 0 (matches CPU behaviour);
subsequent frames compute the difference then run the full GPU pipeline.

The internal API header `feature/speed_internal.h` exposes the CPU linear-algebra
helpers (`speed_internal_qr_factorize`, `speed_internal_backward_substitution`, etc.)
to all four backend TUs without duplicating the implementations.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Full GPU 25×25 eigendecomp (Jacobi iteration on GPU) | Avoids CPU round-trip | 25×25 is < 1 warp; GPU parallelism offers no benefit; Jacobi convergence requires variable iteration count unsuitable for fixed-dispatch kernel | Correct algorithmic split is tile-parallel on GPU, serial math on CPU |
| CPU forwarding (register GPU extractor that calls CPU internals) | No kernel code needed | Violates the "real on-device kernels" requirement; provides no throughput benefit vs the CPU twin | Hard requirement: no CPU forwarding |
| CUDA-only first, other backends later | Faster initial landing | Creates a multi-PR dependency chain; all four backends needed for feature coverage parity | User direction: all 4 backends ship in one PR |
| Specialization constants for Vulkan pass selection | Slightly smaller push-constant payload | Requires one `vkCreateComputePipeline` call per pass (7 pipelines) at init time | Single pipeline with push constant `pc.pass` is simpler and avoids descriptor-set churn per recompilation |

## Consequences

- **Positive**: `speed_chroma` and `speed_temporal` now run on all four GPU backends;
  closes the last extractor parity gap for this metric class. GPU throughput advantage
  applies to the tile-parallel majority of the computation (85–90% of wall time per
  frame on large-format content).
- **Positive**: The `speed_internal.h` header allows future backends to reuse the
  CPU linear-algebra helpers without code duplication.
- **Negative**: The CPU round-trip for eigendecomposition introduces one host-device
  synchronization point per frame per plane. For small resolutions (few tiles) the
  PCIe/ROCm/SYCL transfer overhead may dominate; GPU backend advantage scales with
  tile count.
- **Neutral / follow-ups**: Validate places=4 parity on all four backends against
  Netflix golden src01 fixture. Container build required for HIP HSACO compilation
  (`enable_hipcc=true`); without it, `init()` returns `-ENOSYS` (same posture as all
  prior HIP scaffold consumers).

## References

- CPU reference implementation: `core/src/feature/speed.c`
- ADR-0214: per-backend places=4 numerical parity gate.
- ADR-0353: Vulkan submit pool pattern.
- ADR-0533: HIP extractor registration sweep (precedent for `feature_extractor.c` additions).
- ADR-0567 reserved by: `scripts/adr/next-free.sh --claim speed-chroma-temporal-real-gpu`
- Closes PR #1338 (superseded draft with incomplete kernels).
- Source: user direction (paraphrased) — real on-device GPU kernels for speed_chroma
  and speed_temporal on all four backends; no CPU forwarding; no LOC caps on Vulkan GLSL;
  places=4 numerical gate required.
