<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1167: Row-level rounding accumulator and border row selection for integer ADM GPU kernels

- **Status**: Accepted
- **Date**: 2026-09-03
- **Deciders**: lusoris, Antigravity
- **Tags**: cuda, hip, gpu, feature, integer-adm, numerical-correctness

## Context

An upstream issue harvest triage of Netflix/vmaf#1564 (recorded in `docs/state.md` as
`T-UPSTREAM-1564-ADM-CM-GPU-BORDER-AND-ROUNDING-2026-09-03` and researched in
`docs/research/1166-upstream-issue-harvest-2026-09-03.md`) identified two confirmed
fork-local numerical defects in the VMAFx integer ADM contrast masking kernels (`adm_cm.cu`
and `adm_cm.hip`):

1. **Border row selection at `i == 0 && top <= 0`**: When `i == 0`, `offset_i[0] = 1`.
   The code initialized a running pointer `flt_ptr_line += src_stride * (i + offset_i[0])`
   and then walked it downwards with `flt_ptr_line += src_stride` and
   `flt_ptr_line += src_stride * offset_i[1]`. This caused rows `{1, 2, 3}` to be read
   instead of reflected rows `{1, 0, 1}`, and sampled `csf_a` at row 2 instead of row 0.
2. **Distributed rounding shift across warps/threads**: The CPU reference implementation
   (`core/src/feature/integer_adm.c`) accumulates the full row across all columns
   `j in [start_col, end_col)` into a 64-bit integer sum and applies the rounding shift
   `(row_accum + add_shift_inner_accum) >> shift_inner_accum` exactly once per row.
   In CUDA, the shift was applied inside warp reduction, causing wide frames (e.g. 1920
   pixels wide, spanning 60 warps) to evaluate the rounding bias 60 times per row.
   In HIP, ADR-0539 introduced per-thread atomicAdd with per-thread shifting, evaluating
   the rounding bias 1920 times per row. Because bitwise right-shift is not distributive
   over addition, distributing the rounding shift introduced systematic numerical divergence
   from the CPU reference.

## Decision

1. **Replace running pointers with absolute indexing**: In `adm_cm.cu` and `adm_cm.hip`,
   compute explicit absolute row and column indices (`row_top = i + offset_i[0]`,
   `row_bot = i + offset_i[1]`, `col_l = j + offset_j[0]`, `col_r = j + offset_j[1]`)
   and evaluate `csf_a` at row `i` center `i * src_stride + j`.
2. **Enforce row-level reduction before shifting**: Launch the CM kernels with `gridDim.x = 1`.
   Threads in the block stride across all columns `x in [start_col, end_col)` of the row,
   accumulating values in 64-bit precision. The full row is reduced across the warp/block,
   and the inner accumulation rounding shift `(row_total + add_shift_inner_accum) >> shift_inner_accum`
   is applied exactly once per row before updating `accum_global`.
3. **Supersede ADR-0539**: ADR-0539's premise that per-thread `atomicAdd` after shifting
   is mathematically bit-exact w.r.t. the CPU reference is superseded; row-level reduction
   is mandatory.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Row-level reduction with `gridDim.x = 1` column striding (chosen)** | Matches CPU reference mathematical invariant exactly. Zero distributed rounding drift. Avoids multi-pass kernel launches. | Restricts CTA grid X-dimension to 1, relying on thread striding across columns. | Peak throughput on wide frames remains high; eliminates numerical bias entirely. |
| **Separate multi-pass global grid reduction** | Maximizes thread parallelism across CTA blocks. | Requires temporary scratch buffer allocation, multiple kernel launches, and device synchronization barriers. | Excessive overhead for a 1D row sum; column striding inside the CTA is faster and simpler. |
| **Keep distributed per-warp/per-thread shifting with relaxed tolerance** | No code changes required. | Accumulates systematic bias on large frames; violates the fork's core contract of numerical ground truth. | Unacceptable: numerical correctness overrides convenience. |

## Consequences

- **Positive**: Restores bit-exact numerical parity between CPU, CUDA, and HIP across both small frames (<120 px tall) and wide frames (>=1920 px wide).
- **Negative**: None.
- **Neutral / follow-ups**:
  - Supersedes ADR-0539.
  - Adds `test_cuda_adm_small_border` and `test_cuda_adm_wide_rounding` (and HIP twins) to the test suite.
  - Invariant codified in `core/src/feature/AGENTS.md`.

## References

- Upstream issue: Netflix/vmaf#1564
- Research digest: `docs/research/1166-upstream-issue-harvest-2026-09-03.md`
- Superseded ADR: [ADR-0539](0539-hip-adm-kernels-real.md)
