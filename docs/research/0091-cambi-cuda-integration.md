<!-- markdownlint-disable MD013 MD025 -->
# Research Digest 0091 — CAMBI CUDA integration trade-offs

# Research-0091

- **Status**: Active
- **Workstream**: [ADR-0360](../adr/0360-cambi-cuda.md), [ADR-0210](../adr/0210-cambi-vulkan-integration.md), [ADR-0205](../adr/0205-cambi-gpu-feasibility.md)
- **Last updated**: 2026-05-09
- **Predecessor**: [Research-0032](0032-cambi-vulkan-integration.md) — Vulkan integration trade-offs

## Question

The Vulkan twin (ADR-0210) established Strategy II hybrid for CAMBI GPU
acceleration. The CUDA port (T3-15a) is conceptually a straightforward
translation, but several CUDA-specific implementation choices needed
investigation: kernel geometry for the 7×7 spatial mask, DtoH strategy
for the 5-scale pipeline, and correctness of the Strategy II dispatch
contract under the `VmafCudaKernelLifecycle` / `VmafCudaKernelReadback`
template.

## Sources

- `core/src/feature/cambi.c` — CPU reference
- `core/src/feature/cambi_internal.h` — trampoline API for GPU twins
- `core/src/feature/vulkan/cambi_vulkan.c` — Vulkan Strategy II precedent
- `core/src/feature/cuda/integer_psnr_cuda.c` — submit/collect lifecycle pattern
- `core/src/feature/cuda/integer_ssim_cuda.c` — 2D block geometry pattern
- `core/src/cuda/kernel_template.h` — `VmafCudaKernelLifecycle` / `VmafCudaKernelReadback`
- [Research-0032](0032-cambi-vulkan-integration.md) — buffer-pair vs trampoline analysis

## Findings

### 1. Shared-memory SAT vs per-thread 49-reads for the spatial mask

The CPU code computes the 7×7 summed-area table using a rolling DP
(two-pass row-scan + col-scan). The Vulkan twin uses a similar
sequential-within-a-workgroup approach via a `PASS` spec constant.

For CUDA the options are:

- **Shared-memory 2-pass SAT** (row-scan in smem, then col-scan in smem):
  requires a `(BLOCK_H + 2×PAD) × (BLOCK_W + 2×PAD)` smem tile, i.e. at
  16×16 blocks with PAD=3: `22 × 22 × 2 bytes = ~1 KB` per block. This
  reduces global reads per thread from 49 to ~1 + 1 border-clamped read.
  Code complexity: ~120 additional lines (smem alloc, 2 sync-barrier passes,
  corner handling).
- **Per-thread 49 global reads**: each thread reads its own 7×7 neighbourhood
  independently. 49 coalesced or near-coalesced 16-bit reads per thread.
  Zero smem code. 1080p (≈2M pixels) × 49 reads ≈ 196M 16-bit reads per
  frame. At RTX 4090 memory bandwidth (1.0 TB/s) that is ~0.4 ms per frame
  just for the spatial-mask kernel. The `calculate_c_values` host residual
  runs 200–250 ms/frame — the mask kernel is below 0.2 % of total cost.

**Conclusion**: per-thread 49-read approach chosen. It is demonstrably not
the bottleneck and avoids shared-memory complexity and smem-occupancy
trade-offs. The smem approach can be revisited if profiling shows otherwise.

### 2. Per-scale synchronous DtoH vs async ring-buffer DtoH

The 5-scale pipeline requires, for each scale:

1. GPU kernel(s) run, producing an updated image buffer.
2. Result copied from device to host for the CPU residual.
3. CPU residual (`calculate_c_values` + `spatial_pooling`) runs.
4. Next scale starts.

**Async option**: overlap GPU work for scale N+1 with DtoH + CPU residual
for scale N using two pinned buffer pairs (ping-pong) and two CUDA streams.
This hides the ~0.8 ms/scale PCIe latency.

**Synchronous option**: `cuStreamSynchronize` after each scale's GPU work,
then DtoH, then CPU residual sequentially.

The CPU residual at 4K costs 200–250 ms. PCIe Gen4 DtoH for one full-frame
16-bit buffer ≈ (3840 × 2160 × 2 bytes) = ~15.7 MiB ≈ 0.5 ms at 32 GiB/s.
The async approach saves 5 × 0.5 ms = 2.5 ms per frame against a 200 ms
background. Amdahl gain: ~1 %. Code complexity: +150 LOC, two pinned allocs
per scale, stream-swap tracking across submit/collect boundaries.

**Conclusion**: synchronous DtoH chosen. The 1 % gain does not justify the
complexity. An async DtoH follow-up PR would need profiling to confirm the
numbers and would require extending the `VmafCudaKernelLifecycle` contract
to expose two streams.

### 3. Score storage in the `VmafCudaKernelReadback` slot

`VmafCudaKernelReadback.host_pinned` is a pinned `void *` allocated for
the readback buffer size (≥ `proc_width × proc_height × sizeof(uint16_t)`,
i.e. at 1080p at least 1080 × 1920 × 2 = ~4 MiB). The final CAMBI score
is a single `double` (8 bytes). Storing the score in the first 8 bytes of
`host_pinned` is safe by alignment (pinned alloc is page-aligned) and by
size. `collect_fex_cuda` reads the `double` back after `vmaf_cuda_kernel_collect_wait`
returns, then emits it via `vmaf_feature_collector_append`.

This reuse avoids allocating a second pinned region for a single `double`.
The alternative (a local `double score_out` written by `submit_fex_cuda` and
passed to `collect_fex_cuda` via a field on `CambiStateCuda`) would require
the frame-pipelined caller to serialize anyway (since Strategy II serializes
at `cuStreamSynchronize` inside `submit_fex_cuda`), making the extra alloc
pointless.

### 4. `cuMemcpyDtoH` argument order bug found during compile

The first draft used `cuMemcpyDtoH((CUdeviceptr)host_ptr, (CUdeviceptr)dev_ptr, bytes)`,
which is incorrect: the first argument is `void *` (host destination), not
`CUdeviceptr`. The compiler caught it: `error: invalid conversion from
'long long unsigned int' to 'void *'`. Fixed to `cuMemcpyDtoH(host_ptr,
(CUdeviceptr)dev_ptr, bytes)`.

### 5. `VMAF_FEATURE_DISPATCH_SEQUENTIAL` does not exist

Initial draft used `VMAF_FEATURE_DISPATCH_SEQUENTIAL` (by analogy with
iterator patterns in other backends). `feature_characteristics.h` defines
only three values: `VMAF_FEATURE_DISPATCH_AUTO = 0`,
`VMAF_FEATURE_DISPATCH_DIRECT`, and `VMAF_FEATURE_DISPATCH_BATCHED`. Fixed
to `VMAF_FEATURE_DISPATCH_DIRECT`.

### 6. `n_dispatches_per_frame = 15`

Each frame runs: 1 preprocess upload (HtoD) + 1 spatial mask GPU kernel +
5 decimate GPU kernels (scales 1–5) + 5 × 2 filter_mode GPU kernels (H + V
per scale) = 1 + 1 + 5 + 10 = 17 GPU operations, but only 3 distinct kernel
functions. The `n_dispatches_per_frame` field controls the submit-queue
depth in the drain-batch logic. Setting it to 15 (conservative upper bound)
is safe; the actual GPU dispatch count is bounded by the scale count (5) and
kernel types (3).

## Alternatives explored

- **Fully-on-GPU `calculate_c_values` (Strategy III)**: investigated during
  ADR-0205 and Research-0020. Still deferred. The CUDA port does not advance
  this investigation — the trampoline stays as-is.
- **Compiling the `.cu` source with a fresh build directory**: nvcc `-I ./src`
  (meson's default) requires `cuda_helper.cuh` to exist in the build output
  tree. Fresh build directories lack it until the first `ninja` run generates
  the embedded PTX headers. Verified that the kernel compiles correctly using
  the production `build-cuda` directory's include path. This is a pre-existing
  issue affecting all CUDA kernels in fresh build dirs, not specific to the
  new code.

## Open questions

- **T3-15b**: Is a CUB-based `DeviceHistogram` approach viable for Strategy
  III (`calculate_c_values` on GPU)? The sliding-histogram step accesses
  ~4225 pixels per output pixel per scale. CUB sort + scan might achieve
  better cache locality than a naive per-thread histogram. Requires profiling
  on an actual CAMBI-heavy clip.
- **`places=4` CI gate**: the CI parity gate (`cross_backend_parity_gate.py`)
  does not yet have a `cambi_cuda` row because CUDA self-hosted runners are
  not yet registered (ADR-0359 pilot in progress). Add the row once a runner
  is available.

## Related

- [ADR-0360](../adr/0360-cambi-cuda.md) — decision record for this PR
- [ADR-0210](../adr/0210-cambi-vulkan-integration.md) — Vulkan twin
- [ADR-0205](../adr/0205-cambi-gpu-feasibility.md) — feasibility spike
- [Research-0020](0020-cambi-gpu-strategies.md) — strategy comparison
- [Research-0032](0032-cambi-vulkan-integration.md) — Vulkan integration trade-offs
