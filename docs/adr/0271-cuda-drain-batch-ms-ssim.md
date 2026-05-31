<!-- markdownlint-disable MD060 -->
# ADR-0271: Wire `integer_ms_ssim_cuda` through the CUDA fence-batching helper

- **Status**: Accepted
- **Date**: 2026-05-04
- **Deciders**: Lusoris, Claude
- **Tags**: cuda, gpu, perf, fork-local

## Context

The `integer_ms_ssim_cuda` feature extractor was the loudest remaining
host-blocking caller after the engine-scope CUDA fence-batching helper
(`core/src/cuda/drain_batch.{h,c}`) was introduced. Every other
template-based CUDA extractor (psnr_cuda, …) and the four legacy
extractors (motion, adm, vif, ssimulacra2) already participate in the
batched drain — `vmaf_cuda_drain_batch_flush` waits on every registered
`finished` event in one host-side syscall before the engine enters the
collect-all phase.

`integer_ms_ssim_cuda.c` instead did its work in two places. `submit()`
ran the picture-copy upload + the 4-step decimate pyramid build, then
called `cuStreamSynchronize(s->lc.str)` before returning. `collect()`
walked all 5 SSIM scales sequentially with one `cuStreamSynchronize` per
scale (5 host-blocking syscalls per frame in addition to the one in
`submit()`). The host-side wait pattern was forced by two collisions:

- The intermediate buffers (`h_ref_mu`, `h_cmp_mu`, `h_ref_sq`,
  `h_cmp_sq`, `h_refcmp`) were allocated once at scale 0's size and
  reused across scales. Same-stream ordering serialises kernels by
  CUDA convention so the device side is fine, but the host loop in
  `collect()` had to read the partials each iteration.
- The partials buffers (`l_partials`, `c_partials`, `s_partials`) and
  their pinned-host shadows were single-instance shared across scales.
  Reading scale `i`'s partials on the host before launching scale
  `i+1`'s kernels was the only way to avoid the next launch
  overwriting the buffer mid-DtoH.

The result: 6 host-blocking `cuStreamSynchronize` calls per frame in
the `vmaf_v0.6.1` model path, on top of N-1 syncs from the rest of the
CUDA feature stack — the drain_batch coalesce skipped this extractor
entirely.

## Decision

We allocate **per-scale** partials buffers (5× `l_partials[]` /
`c_partials[]` / `s_partials[]` device buffers and the matching pinned
host shadows), enqueue all 5 scales' `horiz` + `vert_lcs` launches and
DtoH copies back-to-back on the lifecycle's private stream
(`s->lc.str`) inside `submit()`, record `s->lc.finished` once after the
last DtoH, and call `vmaf_cuda_drain_batch_register(&s->lc)` to opt the
lifecycle into the engine's batched drain. `collect()` becomes a
host-side reduction only — `vmaf_cuda_kernel_collect_wait` skips the
per-stream sync when the engine has already drained the event.

The shared SSIM intermediate buffers stay shared. Same-stream ordering
on `s->lc.str` already serialises `horiz` ⇒ `vert_lcs` ⇒ DtoH within a
scale and across scales, so the read-after-write hazard the host loop
worked around is not a hazard on the device side; the only reason the
old code had to break the loop was the host-side reduction stepping in
between launches.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Per-scale partials + drain_batch (this ADR)** | 5+1 host-blocking syncs collapse into the engine's single batched flush; full-frame readback latency hides behind other extractors' kernels; bit-exact (same launches, same order, only host wait point moves). | +12 device buffers and 12 pinned host buffers per state (5×3 + the 3 unused in the old layout, totalling 15 partials + 15 host vs. 3 + 3 before). Footprint increase is dominated by scale 0's block_count (≈ 8.1 KB at 1080p). | Chosen — the latency win dominates, and the buffer footprint is well below the existing pyramid + intermediates allocations. |
| Keep shared partials, single-scale fence each iteration | Smallest device-memory footprint. Bit-exact. | Per-scale `cuStreamSynchronize` stays — drain_batch doesn't help because the events aren't recorded once-per-frame. | Doesn't address the goal — the readbacks stay un-coalesced. |
| Move the per-scale reduction to the GPU (Wang product on-device) | Single scalar DtoH for the final score; smallest possible drain. | Requires a new reduction kernel; numerical fidelity vs. the host-side double accumulation needs a fresh `places=4` cross-backend audit; SYCL/Vulkan twins would diverge or have to follow. | Out of scope — bigger surgery, larger risk; deferred to a future T-GPU-OPT issue. |
| Pre-allocate per-frame in `submit()` instead of `init()` | Zero static footprint when ms_ssim isn't enabled. | Hot-path malloc on every frame; `cuMemAllocHost` round-trips would dwarf the syscall savings. | Strictly worse for the use case. |

## Consequences

- **Positive**: ms_ssim joins the drain_batch coalesce — 6 per-frame
  host-blocking syscalls collapse into the engine's single
  `cuStreamSynchronize(drain_str)`. Expected wall-clock improvement on
  the Netflix CUDA benchmark for the ms_ssim feature path: +3-5%
  (consistent with the per-frame syscall counts and with the savings
  measured for psnr_cuda when it migrated to the same pattern).
  Bit-exact: same kernels, same stream, same submission order; only
  the host-side wait point moves. The extractor stays correct when
  no batch is open (engine in legacy mode, unit tests exercising
  `submit`/`collect` directly) — `vmaf_cuda_drain_batch_register` is
  a no-op outside a batch and `vmaf_cuda_kernel_collect_wait` falls
  back to the per-stream sync.
- **Negative**: device-memory footprint grows by ~12 small buffers per
  state. At 1080p that's ≈ 8.1 KB of device + 8.1 KB of pinned host
  per side; negligible against the existing pyramid + intermediates
  allocations (~ 10 MB at 1080p).
- **Neutral / follow-ups**:
  - The Vulkan and SYCL twins (`integer_ms_ssim_vulkan.c`,
    `integer_ms_ssim_sycl.cpp`) keep their per-frame
    collect/submit ordering unchanged — `drain_batch` is CUDA-only
    by design (per-backend, see ADR-0246). No SYCL/Vulkan parity work
    required.
  - Cross-backend `places=4` gate (ADR-0214) keeps watching the
    SYCL ↔ CUDA delta. No threshold relaxation.

## References

- T-GPU-OPT-2 (this ADR's task identifier).
- `core/src/cuda/drain_batch.{h,c}` — the helper and its
  bit-exactness invariant (places=4).
- `core/src/cuda/kernel_template.h` — `vmaf_cuda_kernel_collect_wait`
  fast path that participating extractors short-circuit through.
- [ADR-0246](0246-gpu-kernel-template.md) — per-backend GPU kernel
  scaffolding template (CUDA template introduced).
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend parity gate.
- Reference consumer: `core/src/feature/cuda/integer_psnr_cuda.c` —
  pattern this ADR follows verbatim for the lifecycle hand-off.
- Source: `req` (direct user instruction in the agent task brief).
