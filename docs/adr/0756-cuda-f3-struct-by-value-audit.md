<!-- markdownlint-disable MD060 -->
<!--
  Copyright 2026 Lusoris
  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
-->
# ADR-0756: CUDA F3 struct-by-value kernel audit (scope + dispatch order)

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `perf`, `research`

## Context

PR #93 identified "F3" — a pattern where a `__global__` kernel accepts a
`VmafCudaBuffer` (or `VmafPicture`, `AdmBufferCuda`) by value. Because
`VmafCudaBuffer.data` is a `CUdeviceptr` (opaque integer) nested inside a
struct copy, ptxas cannot infer that the underlying pointer is non-aliased and
cannot emit `ld.global.nc` (the read-only L1 texture path). PR #93 applied
the in-kernel `__ldg()` extraction fix to `calculate_ssim_vert_combine` and
measured -4.2% kernel duration at 1080p (Research-0754).

The scope of remaining instances across the CUDA kernel suite was not
enumerated before PR #93 merged. This ADR records the fork-wide audit result
and the chosen dispatch order for follow-on PRs.

## Decision

We will address the F3 pattern using the in-kernel `__ldg()` extraction
strategy (extract raw `const T *` from each struct arg before the hot inner
loop; read via `__ldg()`). We will apply this in severity order per
Research-0756: `ms_ssim_vert_lcs` first (PR-1), then `ms_ssim_horiz`,
`ciede_8bpc/16bpc`, `adm_decouple`. Kernels whose inner loop is already
smem-resident after the tile-load phase (motion, adm_cm) are explicitly
out of scope for F3 treatment — the DRAM-bound portion is the tile load,
not a repeated inner-loop global read.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Host-side extraction (pass raw `CUdeviceptr` to `cuLaunchKernel`) | Full compiler visibility; enables ptxas to use `ld.global.nc` without `__ldg()` | Requires modifying every call site in `_cuda.c` dispatch files; higher risk, larger diff | Deferred; revisit if post-F3 ncu shows remaining DRAM headroom |
| AoS → SoA buffer restructure (F1) | Best long-term coalescing | Structural change to `VmafCudaBuffer`; API impact | Deferred per Research-0754 decision section |
| Skip F3 for all kernels that are launch-starved at 576p | Correct that at 576p F3 rarely matters | Ignores 1080p+ production workloads | Not chosen; fork targets 1080p/4K production |

## Consequences

- **Positive**: Each dispatched PR carries at most 30–40 LOC diff, is
  bit-identical (zero score drift), and is independently deployable. The
  in-kernel pattern was proven by PR #93 with a live ncu A/B.
- **Negative**: Multiple separate PRs rather than one large refactor.
  Call-site code in `_cuda.c` files still passes structs; the compiler
  sees the `__ldg()` hint but not full `__restrict__` aliasing. Full
  benefit requires the eventual host-side extraction pass.

## References

- req: "Per PR #93: this affects every kernel that takes its inputs as
  VmafCudaBuffer struct copies" (per user direction, 2026-05-29)
- Research-0754 (`calculate_ssim_vert_combine` ncu A/B)
- Research-0756 (this audit)
- Research-0736 (SSIM ncu hotpath)
- Research-0737 (MS-SSIM ncu hotpath)
- Research-0734 (ADM ncu hotpath)
- ADR-0108 (deep-dive deliverables)
