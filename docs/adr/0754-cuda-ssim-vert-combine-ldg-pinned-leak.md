<!-- markdownlint-disable MD013 MD060 -->
# ADR-0754 — CUDA SSIM `vert_combine`: `__ldg()` + `__launch_bounds__` + pinned-host leak fix

| Field  | Value |
| ------ | ----- |
| Status | Accepted |
| Date   | 2026-05-29 |
| Tags   | cuda, performance, correctness, ssim, fork-local |

## Context

A code review of `calculate_ssim_vert_combine` in
`core/src/feature/cuda/integer_ssim/ssim_score.cu` surfaced three findings
suitable for a single low-risk PR (ordered lowest to highest risk below).

**F4 — `__launch_bounds__(128)` (occupancy hint).**
The kernel launches with 16×8 = 128 threads/block. Without a
`__launch_bounds__` annotation, nvcc may allocate more registers than needed
for 128-thread occupancy. Adding `__launch_bounds__(128)` sets an upper register
budget consistent with the actual configuration and follows the pattern
established in ADR-0743 for the VIF filter1d kernel.

**F2 — `__ldg()` on the 5 read-only intermediate buffers.**
`calculate_ssim_vert_combine` reads 5 float buffers (h_ref_mu, h_cmp_mu,
h_ref_sq, h_cmp_sq, h_refcmp) that are written exclusively by the preceding
horizontal pass and never aliased in the vertical pass. These 5x11 = 55
inner-loop loads are therefore good candidates for the non-coherent read-only
path (texture cache). However, the buffers are passed as `VmafCudaBuffer`
structs by value; without `__restrict__`, the compiler cannot determine the
underlying pointers are non-aliased. Fix: extract `const float *__restrict__`
pointers once before the inner loop, then use `__ldg(&ptr[idx])` for every
load. The compiler can now route all 55 loads through the L1 read-only cache,
reducing L2 pressure at resolution >= 1080p where the combined 5-plane
intermediate footprint exceeds L2 capacity (mirrors ADR-0743 note on VIF
tmp buffers).

**F6 — pinned-host memory leak in `close_fex_cuda` (correctness).**
`vmaf_cuda_kernel_readback_free` (in `cuda/kernel_template.h`) explicitly
NULLs `rb->host_pinned` but does NOT free the underlying CUDA pinned host
allocation — the template comment documents this as a caller responsibility.
`integer_ssim_cuda.c::close_fex_cuda` called `readback_free` without first
saving the host pointer, losing the only reference needed to call
`vmaf_cuda_buffer_host_free`. Result: one page of pinned host memory leaked
per `vmaf_close()` cycle. Fix: save `rb.host_pinned` to a local before
`readback_free`, then call `vmaf_cuda_buffer_host_free(fex->cu_state, saved)`.

**Deferred findings (not in this PR):**

- F1 (AoS to SoA buffer pack) — bigger change; revisit if F2 does not move
  the needle enough at 1080p.
- F3 (kernel signature change paired with F1) — dependent on F1.
- F5, F7, F8 — cosmetic or separate-concern; different PRs.
- `integer_psnr_cuda.c` has the same `readback_free` / `host_free` gap and
  is explicitly named for a follow-up fix.

## Decision

Apply F4, F2, and F6 together in this PR:

1. Add `__launch_bounds__(128)` to `calculate_ssim_vert_combine`.
2. Extract `const float *__restrict__` pointers from the 5 `VmafCudaBuffer`
   arguments before the inner loop; use `__ldg(&ptr[idx])` for all 55 loads.
3. In `close_fex_cuda`, save `s->rb.host_pinned` before `readback_free`,
   then call `vmaf_cuda_buffer_host_free(fex->cu_state, saved_host_pinned)`.

F6 is unconditional (memory-safety fix). F4 + F2 are abandoned if ncu
measurements show regression (per the ms_ssim_decimate lesson, Research-0749).

## Alternatives considered

| Option | Considered | Outcome |
| ------ | ---------- | ------- |
| F2 only (skip F4) | Yes | F4 is two characters with zero risk; adds occupancy-budget symmetry with ADR-0743. Include. |
| F2 + F4 only (skip F1/F3) | Yes — adopted | AoS to SoA (F1) is a larger change. Phase it: measure if F2 moves the needle at 1080p; if not, revisit F1. |
| F1 + F2 + F3 + F4 full rewrite | Yes | Touching both `.cu` and `.c` in a restructure PR increases review risk. Phased approach preferred. |
| Fix `readback_free` itself to call `host_free` | Yes | Changes the template's documented contract; requires a broader caller audit. Per-caller fix is lower-risk for now. |

## Consequences

- `calculate_ssim_vert_combine` carries `__launch_bounds__(128)`.
- All 55 inner-loop loads route through the read-only texture cache.
  L2 pressure is reduced at >= 1080p.
- `close_fex_cuda` no longer leaks pinned host memory.
- `integer_psnr_cuda.c` has the same readback_free / host_free gap and is
  named for a follow-up fix.
- Invariant notes added to `core/src/feature/cuda/AGENTS.md`:
  - `__ldg()` pattern for read-only intermediate buffers in pass-2 kernels.
  - Pinned-host memory free responsibility after `readback_free`.

## References

- req: user direction 2026-05-29: "Implement the 3 actionable findings from
  the SSIM vert_combine review report."
- ADR-0743: VIF filter1d `__launch_bounds__` + `__ldg()` precedent.
- ADR-0246: CUDA kernel-template lifecycle contract.
- ADR-0214: GPU-parity CI gate (places=4).
- `cuda/kernel_template.h`: comment near `vmaf_cuda_kernel_readback_free`
  documenting the host-free caller responsibility.
