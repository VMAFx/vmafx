<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1204: GPU ADM contrast-masking twins clamp the far edge instead of mirroring it

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, hip, metal, correctness, feature-extractor, testing

## Context

The CPU float-ADM contrast-masking threshold is a 3x3 neighbourhood sum whose
edge policy is **asymmetric**. `adm_cm_thresh3x3_s`
(`core/src/feature/adm_tools.c`) is the closed form of the nine
`ADM_CM_THRESH_S_*` macro variants and reads:

```c
i_m1 = (i == 0)     ? 1     : i - 1;   /* near edge MIRRORS to index 1   */
i_p1 = (i == h - 1) ? h - 1 : i + 1;   /* far  edge CLAMPS to last index */
```

All four GPU twins implemented the far edge as a *mirror* instead
(`x = 2 * half_w - x - 2`, i.e. `w - 2`), so their neighbourhood sums read a
different sample than the CPU reference at the last row and last column.

The defect was invisible for two reasons. First, it only changes the result
when the ADM border crop collapses to zero: the crop is
`(int)(dim * ADM_BORDER_FACTOR - 0.5)`, which is `0` for `dim <= 14` and `>= 1`
above, and only a zero crop puts row 0 / row `h-1` / col 0 / col `w-1` inside
the summation region. Second, `test_cuda_float_adm_parity` pins a single
256x144 fixture, and CI runners have no CUDA device, so the test took its
`[skip: no CUDA device]` path and reported green. On real hardware it was red.

Measured on an RTX 4090, `VMAF_feature_adm_scale3_score` diverged by
**8.58e-04** at 256x144 against a places=4 (1e-4) gate, while every scale at
512x288 and above agreed to ~1e-8. A width/height sweep isolated the trigger
exactly: divergence appears if and only if the scale-3 band dimension is
`<= 14`, independently for width and for height — precisely the zero-crop
condition.

## Decision

We will make all four GPU twins (CUDA, SYCL, HIP, Metal) clamp the far edge to
the last index, matching the CPU closed form. Reads are only ever at `+/-1`, so
`x = half_w - 1` is the exact CPU semantics, not an approximation.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Clamp the far edge in the GPU twins (chosen) | Restores CPU parity exactly; one-line change per backend; no CPU behaviour change | Four files to keep in sync | — |
| Change the CPU to mirror the far edge | Would also produce parity, single file | Changes the shipped CPU reference and therefore every published ADM score; the CPU is the golden side | Rejected — the CPU is the reference, not the thing to bend |
| Widen the parity tolerance past 8.6e-04 | Zero code change | Hides a real indexing bug and weakens a correctness gate | Rejected — the fork forbids weakening gates to hide defects |

## Consequences

- **Positive**: `float_adm_cuda` matches CPU `float_adm` to ~4e-08 at every
  resolution tested (was 8.58e-04 at small band sizes). The whole shipped CUDA
  parity suite goes 16/18 -> 18/18 on real hardware.
- **Negative**: none measured. Only the zero-crop border pixels change, and
  they change *towards* the CPU reference.
- **Neutral / follow-ups**: SYCL, HIP and Metal carry the identical fix but are
  verified by their own CI parity lanes (`SYCL Parity (Arc A380)`, Ubuntu HIP,
  macOS Metal) rather than locally — this workstation only has a CUDA device
  free. The resolution blind spot that hid this is addressed separately by
  [ADR-1206](1206-gpu-parity-large-fixture-variants.md).

## References

- CPU reference: `core/src/feature/adm_tools.c::adm_cm_thresh3x3_s`.
- Same defect class in the integer ADM GPU kernels, fixed earlier by
  [ADR-1167](1167-adm-cm-row-level-rounding.md) / PR #1224;
  the float-ADM twins were never given the matching fix.
- Reproducer: `meson test -C build --suite=gpu test_cuda_float_adm_parity`
  on a CUDA host, or the width/height sweep in the PR description.
- Source: `req` — user direction to fix the outstanding GPU parity failures.
