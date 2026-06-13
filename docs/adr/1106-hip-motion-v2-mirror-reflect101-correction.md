<!-- markdownlint-disable MD013 MD060 -->
# ADR-1106: HIP motion_v2 mirror is reflect-101 (`-2`), correcting ADR-0377's `-1` parity claim

- **Status**: Accepted
- **Date**: 2026-06-13
- **Deciders**: Lusoris
- **Tags**: `hip`, `gpu`, `motion-v2`, `parity`, `boundary`, `bug-fix`, `fork-local`

## Context

[ADR-0377](0377-hip-batch4-ciede-motion-v2.md) shipped the HIP
`integer_motion_v2` kernel and asserted (body, "differs from
`motion_hip`'s skip-boundary mirror"):

> Mirror padding uses reflective mirror (`2 * size - idx - 1`) … matching
> the CPU and CUDA motion_v2 references.

That parity claim is **incorrect**. The CPU, CUDA, and SYCL motion_v2
kernels all use reflect-101, `2 * size - idx - 2`, at the high boundary:

- CPU `core/src/feature/integer_motion_v2.c:157` — `return 2 * size - idx - 2;`
- CUDA `core/src/feature/cuda/integer_motion_v2/motion_v2_score.cu:51` — `return 2 * sup - idx - 2;`
- SYCL `core/src/feature/sycl/integer_motion_v2_sycl.cpp:95` — `return 2 * sup - idx - 2;`

The HIP kernel used `2 * sup - idx - 1`. All four backends call the mirror
identically — `mv2_mirror(tile_origin + tid, dim)` — so there is no
indexing-convention difference that would make `-1` and `-2` equivalent:
for any out-of-bounds coordinate the HIP kernel reflected to a pixel one
column/row off from the CPU/CUDA/SYCL reference. This is the same class of
silent boundary divergence corrected for HIP integer-VIF in
[ADR-1103](1103-hip-vif-mirror2-boundary.md). The CUDA twin's own comment
states it matches `integer_motion_v2.c::mirror` (which is `-2`); the HIP
comment had copied the wording while diverging the formula.

The divergence affects only pixels reflected across the frame boundary, so
it is content-dependent and was not caught by the existing parity sampling.
It is **HIP-only** and does **not** touch the CPU Netflix golden numbers
(the golden gate is CPU-only).

## Decision

Change the HIP `mv2_mirror` high-boundary case from `2 * sup - idx - 1` to
`2 * sup - idx - 2`, making it byte-identical to the CUDA/SYCL twins and the
CPU reference, and correct the kernel header comment that wrongly claimed
the `-1` form matched CPU/CUDA. This ADR supersedes the
`2 * size - idx - 1` parity claim in ADR-0377; the rest of ADR-0377 (kernel
structure, batch path, ciede) stands.

## Alternatives considered

- **Keep `-1`, "fix" CPU/CUDA/SYCL to match.** Rejected: the CPU kernel is
  the reference for the cross-backend parity contract (ADR-0214) and feeds
  the golden gate; changing it would shift validated scores. HIP is the
  outlier and must conform.
- **Leave it (HIP-only, sub-pixel).** Rejected: it is a real correctness
  divergence with a false in-code/ADR parity claim; the fork's standard is
  to fix the bug and correct the claim, not document around it.

## Consequences

- **Positive**: HIP motion_v2 boundary handling now matches CPU/CUDA/SYCL
  exactly; one less latent cross-backend divergence; the misleading comment
  and ADR claim are corrected.
- **Negative**: HIP motion_v2 scores on content with boundary motion shift
  slightly (toward the reference). No CPU/golden impact.
- **Neutral / follow-ups**: Full device cross-backend-diff
  (`/cross-backend-diff`, places=4 per ADR-0214) for motion_v2 should run in
  the `vmaf-dev-mcp` container — the host gfx1036 iGPU HIP runtime errored
  on the parity run (host HIP-runtime debt, CLAUDE.md §15; the container is
  the supported HIP path). The fix is correct-by-construction (formula now
  identical to the parity-verified CUDA/SYCL twins) and compiles under
  `hipcc`; the container run is confirmation, not a gate.

## Supply-chain impact

- **New dependencies**: none.
- **Removed dependencies**: none.
- **Build-time fetches**: none.

## References

- Supersedes the parity claim in
  [ADR-0377](0377-hip-batch4-ciede-motion-v2.md) (HIP batch-4 ciede +
  motion_v2).
- Sibling boundary correction: [ADR-1103](1103-hip-vif-mirror2-boundary.md)
  (HIP integer-VIF clamp→mirror2).
- Cross-backend parity contract: [ADR-0214](0214-gpu-parity-ci-gate.md).
- Source: surfaced by an adversarial fresh-eyes verification sweep; root
  cause confirmed by reading the mirror formula and call sites across CPU,
  CUDA, SYCL, and HIP.
