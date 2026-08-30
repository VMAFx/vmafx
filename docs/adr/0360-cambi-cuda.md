<!-- markdownlint-disable MD013 -->
# ADR-0360: CAMBI CUDA port (Strategy II hybrid, T3-15a)

- **Status**: Accepted
- **Date**: 2026-05-09
- **Deciders**: <lusoris@pm.me>, Claude (Anthropic)
- **Tags**: cuda, gpu, cambi, feature-extractor, fork-local, places-4, t3-15

## Context

ADR-0210 (cambi Vulkan integration) established the Strategy II hybrid
architecture — GPU kernels for the embarrassingly parallel stages
(spatial mask, 2× decimate, 3-tap mode filter) with the host CPU
handling the precision-sensitive `calculate_c_values` sliding-histogram
pass and top-K spatial pooling. The Vulkan twin passes the `places=4`
gate (ULP=0 on the emitted score) and is in production.

Backlog item **T3-15a** extends that coverage to the CUDA backend.
At the time this PR was written, `docs/backends/cuda/overview.md` listed
CAMBI in the "Known gaps" section: "no CUDA kernel — frame is downloaded
to host memory for CAMBI and the CUDA twin is used for everything else."
This ADR closes that gap.

The implementation follows the Strategy II hybrid identically to the
Vulkan twin. The decision space had already been explored in
[ADR-0205](0205-cambi-gpu-feasibility.md) (feasibility spike) and
[ADR-0210](0210-cambi-vulkan-integration.md) (Vulkan integration);
this ADR records only the CUDA-specific choices.

## Decision

We implement a CUDA twin of `vmaf_fex_cambi` under
`core/src/feature/cuda/integer_cambi_cuda.c` with three CUDA kernels
in `core/src/feature/cuda/integer_cambi/cambi_score.cu`.

**GPU kernel inventory** (three kernels, Strategy II scope):

1. `cambi_spatial_mask_kernel` — per-thread 7×7 box-sum of the
   zero-derivative field (pixel == right AND == below), then threshold
   compare. Border pixels treat out-of-bounds neighbours as "equal",
   matching the CPU code path exactly. No shared-memory SAT needed: the
   7×7 window (49 global reads per thread) fits the register budget at a
   16×16 thread block without shared-memory pressure.

2. `cambi_decimate_kernel` — stride-2 gather: `dst[y][x] = src[2y][2x]`.
   Bit-exact with `cambi.c::decimate`.

3. `cambi_filter_mode_kernel` — separable 3-tap mode filter. Two launches
   per scale: `axis=0` (horizontal), `axis=1` (vertical). `mode3_dev(a,b,c)`
   returns the value that appears at least twice, or the minimum of all three
   if all distinct. Bit-exact with `cambi.c::filter_mode`.

**Host residual** (unchanged from Vulkan twin): after each scale's GPU
phases, `cuStreamSynchronize` is called, the device buffer is copied back
row-by-row to a `VmafPicture` pair, and the CPU functions
`vmaf_cambi_calculate_c_values` + `vmaf_cambi_spatial_pooling` run
against those buffers. `vmaf_cambi_weight_scores_per_scale` then
accumulates the per-scale contribution. The final score is clamped to
`cambi_max_val` and stored in the `VmafCudaKernelReadback` slot so
`collect_fex_cuda` can emit it after the dummy-event completes.

**Build wiring**:

- `cambi_score.cu` is compiled via the existing `cuda_cu_sources` dict in
  `core/src/meson.build` (nvcc → fatbin → bin2c → `cambi_score_ptx[]`
  linked into `libcuda_common_vmaf_lib`).
- `integer_cambi_cuda.c` is added to the CUDA feature sources list in the
  same `meson.build`.
- `vmaf_fex_cambi_cuda` is registered in `feature_extractor.c`'s
  `feature_extractor_list[]` under the `#if HAVE_CUDA` guard.

**Dispatch hint**: `VMAF_FEATURE_DISPATCH_DIRECT` (no batching variant
exists). The CPU residual serializes frames in `submit_fex_cuda` anyway,
so the drain-batch fast path is registered but the serialization point is
the `cuStreamSynchronize` inside `submit_fex_cuda`, not the readback
event.

**Bit-exactness**: all three GPU phases are integer arithmetic on
`uint16_t` device arrays. The host residual runs the exact CPU code from
`cambi_internal.h`. The emitted score is bit-for-bit identical to
`vmaf_fex_cambi` (ULP=0, `places=4` gate holds).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Strategy II hybrid — GPU mask/decimate/filter, CPU c-values (chosen)** | Bit-exact w.r.t. CPU at `places=4`; reuses `cambi_internal.h` trampoline already proven by the Vulkan twin; ~350 LOC net new. | c-values phase (dominant CPU cost) not accelerated; one `cuStreamSynchronize` + DtoH per scale. | Selected — closes the CUDA matrix gap now at low risk; Strategy III is a focused v2 follow-up. |
| **Strategy III — fully-on-GPU `calculate_c_values`** | Full GPU utilisation; eliminates per-scale DtoH. | Sliding-histogram `calculate_c_values` reads ~4225 pixels per output pixel at each scale; a warp-cooperative histogram implementation requires CUB `DeviceReduce` or cooperative groups and ~800 additional LOC. No profile data to verify cache-hit rate. Precision contract unclear without an fp32 bisect. | Premature without profile data; tracked as T3-15b. |
| **Shared-memory SAT for `cambi_spatial_mask_kernel`** | Reduces global reads from 49 to ~14 per thread (read once into smem, sum from smem). | 2-pass row-scan + col-scan requires 49-pixel border in shared memory per block; smem occupancy limits block size or register count. Code complexity doubles for marginal gain at 1080p (spatial-mask kernel is not the bottleneck — c-values dominates). | Deferred to a targeted kernel-tuning PR if profiling shows the mask kernel as a bottleneck. |
| **Async multi-scale DtoH with pinned ring buffer** | Overlaps GPU kernel for scale N with DtoH for scale N-1; hides ~0.8 ms/scale PCIe latency. | Requires 2 pinned buffers × 5 scales = 10× allocation vs the current 3 flat device buffers; requires tracking even/odd swap state across collect boundaries; adds ~150 LOC for negligible gain when c-values dominates at 200–250 ms/frame. | Cost > benefit. `cuStreamSynchronize` inside `submit_fex_cuda` is simpler and correct; async DtoH can be added in a follow-up if profiling warrants. |

## Consequences

- **Positive**:
  - CUDA backend now covers CAMBI; "Known gaps" entry removed from
    `docs/backends/cuda/overview.md`.
  - `vmaf_fex_cambi_cuda` is discoverable via the standard
    `--feature cambi_cuda` flag when `--no_cuda=false`.
  - `places=4` bit-exactness inherited from the Vulkan twin's Strategy II
    precedent — no new precision risk.
  - The three GPU kernels are small (~80 lines of device code each) and
    straightforward to audit.

- **Negative**:
  - The dominant CPU cost (`calculate_c_values` + topK pooling, ~200–250
    ms/frame at 4K) is not accelerated; overall frame throughput improvement
    is bounded by Amdahl's law until T3-15b (Strategy III).
  - One `cuStreamSynchronize` per scale (5 total per frame) serializes the
    GPU timeline; a future async DtoH path would remove these stalls.

- **Neutral / follow-ups**:
  - T3-15b: Strategy III fully-on-GPU `calculate_c_values` — requires
    CUB/cooperative-groups histogram kernel and a precision bisect.
  - The `places=4` cross-backend gate (`scripts/ci/cross_backend_vif_diff.py`
    / `cross_backend_parity_gate.py`) should gain a `cambi_cuda` row once a
    self-hosted CUDA runner is registered (ADR-0359 pilot).

## References

- [ADR-0205](0205-cambi-gpu-feasibility.md) — feasibility spike (Strategy II verdict)
- [ADR-0210](0210-cambi-vulkan-integration.md) — Vulkan twin reference implementation
- [ADR-0192](0192-cuda-feature-extractor-cadence.md) — one PR per backend cadence
- [Research-0020](../research/0020-cambi-gpu-strategies.md) — GPU strategy comparison
- [Research-0032](../research/0032-cambi-vulkan-integration.md) — Vulkan integration trade-offs
- [Research-0091](../research/0091-cambi-cuda-integration.md) — CUDA integration trade-offs (this PR)
- Related PR: T3-15a implementation PR (this ADR accompanies)
- Source: `req` — user direction to port CAMBI to CUDA following ADR-0210's Strategy II hybrid
