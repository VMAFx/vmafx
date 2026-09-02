<!-- markdownlint-disable MD013 MD060 -->
# ADR-0757 — CUDA MS-SSIM `ms_ssim_vert_lcs` + `ms_ssim_horiz`: `__ldg()` + `__launch_bounds__` (F3 fix #2)

| Field  | Value |
| ------ | ----- |
| Status | Accepted |
| Date   | 2026-05-29 |
| Tags   | cuda, performance, ms_ssim, fork-local |

## Context

The PR #96 audit of `core/src/feature/cuda/integer_ms_ssim/ms_ssim_score.cu`
identified two kernels as the top candidates for the F3 fix pattern first
applied in ADR-0754 (`calculate_ssim_vert_combine`):

**`ms_ssim_horiz`** — horizontal 11-tap separable Gaussian over ref / cmp.
The kernel passes 7 `VmafCudaBuffer` arguments by value; two of them (`ref_in`,
`cmp_in`) are read-only input planes written exclusively by the upload pass.
All 2×11 = 22 inner-loop loads used plain dereferencing rather than `__ldg()`,
preventing the compiler from routing them through the read-only L1 texture cache.

**`ms_ssim_vert_lcs`** — vertical 11-tap pass on 5 horizontal-pass intermediate
buffers + per-pixel l/c/s + per-block partial sums. The 5 intermediate buffers
(`h_ref_mu`, `h_cmp_mu`, `h_ref_sq`, `h_cmp_sq`, `h_refcmp`) are written
exclusively by `ms_ssim_horiz` and are never aliased in the vert pass. However,
they were extracted as plain `const float *` (no `__restrict__`), which hides
the alias-free invariant from the compiler. All 5×11 = 55 inner-loop loads
therefore went through L2 rather than the read-only L1 cache.

Neither kernel carried a `__launch_bounds__` annotation despite launching with
16×8 = 128 threads/block.

The fix pattern is identical to ADR-0754 (PR #93):

1. Add `__launch_bounds__(128)` to both kernels.
2. Extract `const float *__restrict__` pointers from every `VmafCudaBuffer`
   argument before the inner loop.
3. Replace all inner-loop loads with `__ldg(&ptr[idx])`.

SASS verification via `cuobjdump --dump-sass` confirmed `LDG.E.CONSTANT`
instructions in both kernel bodies after applying the fix.

Predicted latency reduction at 1080p (from PR #96 audit): −4 to −6% per kernel
for `ms_ssim_vert_lcs`; similar for `ms_ssim_horiz`. Both are memory-bound at
≥1080p where the combined 5-plane (or 2-plane) intermediate footprint exceeds L2
capacity.

## Decision

Apply the F3 fix to both kernels in the same PR:

1. Add `__launch_bounds__(128)` to `ms_ssim_horiz` and `ms_ssim_vert_lcs`.
2. In `ms_ssim_horiz`: extract `const float *__restrict__ ref` and
   `const float *__restrict__ cmp` from `ref_in.data` / `cmp_in.data` before the
   K=11 loop; use `__ldg(&ref[src_idx])` and `__ldg(&cmp[src_idx])` for all 22
   inner-loop loads.
3. In `ms_ssim_vert_lcs`: extract all five `const float *__restrict__` pointers
   before the K=11 loop; use `__ldg(&ptr[src_idx])` for all 55 inner-loop loads.
4. Rename local variable `w` → `gw` in both loops to avoid shadowing the kernel
   parameter `width` and improve readability.

## Alternatives considered

| Option | Considered | Outcome |
| ------ | ---------- | ------- |
| F3 on `vert_lcs` only | Yes | `ms_ssim_horiz` has the same alias-hiding pattern and is memory-bound at the same resolution. Include both — 6 extra lines, zero added risk. |
| Shared-memory tiling (ADR-0464 pattern) | Yes | ms_ssim kernels are separable 11-tap Gaussian; spatial overlap is moderate (K/2 = 5 taps). Profiling at 576p shows wave-starvation dominates over memory latency at that resolution. Tiling is a separate investigation item; `__ldg()` is the lower-risk first step per ADR-0754 precedent. |
| AoS → SoA buffer layout (F1) | Yes | Larger change; deferred pending measurement of F3 impact as in ADR-0754. |
| Skip `__launch_bounds__` | No | Two characters, zero risk; consistent with ADR-0754 and ADR-0743 precedents. |

## Consequences

- `ms_ssim_horiz` and `ms_ssim_vert_lcs` carry `__launch_bounds__(128)`.
- All 22 + 55 = 77 inner-loop loads route through the read-only L1 texture cache
  (`LDG.E.CONSTANT` confirmed in sm_89 SASS).
- L2 pressure is reduced at ≥1080p where the 2-plane or 5-plane intermediate
  footprint exceeds L2 capacity.
- Zero ULP divergence expected (per ADR-0754 precedent; correctness path is
  fully determined by the Gaussian weight table; `__ldg()` does not alter the
  loaded value).
- `AGENTS.md` invariant note under `__ldg() pattern for pass-2 read-only
  intermediate buffers (ADR-0754)` updated to note that ms_ssim is the second
  application of this pattern.

## References

- req: user direction 2026-05-29: "Apply the F3 fix to the top-2 candidates
  from PR #96's audit: ms_ssim_vert_lcs and ms_ssim_horiz. Mirror exactly what
  PR #93 did for calculate_ssim_vert_combine."
- ADR-0754: F3 pattern precedent on `calculate_ssim_vert_combine`.
- ADR-0743: `__ldg()` and `__launch_bounds__` precedent on VIF filter1d.
- ADR-0214: GPU-parity CI gate (places=4).
- Research-0749 / ADR-0750: ms_ssim_decimate profiling baseline (ms_ssim_horiz
  and ms_ssim_vert_lcs were identified as memory-bound at 1080p in that audit).
