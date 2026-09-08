<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1215: The 16-bpc CUDA PSNR kernel takes the plane index the host has always passed

- **Status**: Proposed
- **Date**: 2026-09-07
- **Deciders**: Lusoris
- **Tags**: cuda, correctness, feature-extractor, bit-depth

## Context

`psnr_cuda` computed the wrong chroma PSNR for every input above 8 bits.
Measured on `src01_hrc00/hrc01_576x324`, frame 0:

| | `psnr_y` | `psnr_cb` | `psnr_cr` |
|---|---|---|---|
| CPU, 10 bpc | 34.786288 | 39.255496 | 41.375212 |
| CUDA, 10 bpc (before) | 34.786288 | **34.358981** | **34.358981** |
| CUDA, 12 bpc (before) | 34.792654 | **34.365347** | **34.365347** |
| CPU / CUDA, 8 bpc | identical on all three planes | | |

`psnr_cb == psnr_cr`, and both are a luma-like number: the chroma dispatches
were measuring a chroma-sized top-left window of the **luma** plane.

`psnr_cuda_dispatch` launches one kernel per plane and passes
`{ref, dis, sse, width, height, plane}` for both bit depths. The 8-bpc kernel
declares the `plane` parameter and indexes `data[plane]` / `stride[plane]`.
The 16-bpc kernel was declared without it and hard-coded index 0 — the
driver API silently ignores the surplus argument, so nothing failed loudly.
SYCL and HIP index by plane correctly.

Every PSNR parity fixture was 8-bit with flat chroma, so `psnr_cb` / `psnr_cr`
sat at the `psnr_max` sentinel on both sides and the wrong-plane read could
never have shown up.

## Decision

We will give `calculate_psnr_kernel_16bpc` the `plane` parameter and index by
it, exactly as the 8-bpc kernel does. The parity fixture is made bit-depth
generic — the 8-bit fixture is kept byte-identical and widened into a
`FIXTURE_BPC` picture above 8 bpc, with chroma made non-flat and different
between ref and dist so that the chroma planes carry a real signal — and the
TU is registered again at 10 bpc.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add the `plane` parameter to the 16-bpc kernel (chosen) | Three-line kernel change; the host needs no change because it already passes the argument | — | — |
| Launch three per-plane kernels with pre-offset base pointers from the host | Kernel stays plane-agnostic | Changes the host for both bit depths to fix one kernel's missing parameter | Rejected |
| Keep flat chroma in the 10-bit fixture | Smaller test diff | Both sides report the `psnr_max` sentinel for flat chroma, so the wrong-plane read stays invisible — the exact blind spot that let this ship | Rejected |

## Consequences

- **Positive**: CUDA `psnr_cb` / `psnr_cr` match the CPU to the printed six
  decimals at 10 and 12 bpc; `test_cuda_psnr_parity_10bit` passes on an RTX
  4090 alongside the unchanged 8-bit test.
- **Negative**: any artifact that consumed CUDA chroma PSNR above 8 bpc
  consumed a luma number. No fork-added snapshot does.
- **Neutral / follow-ups**: the sweep also flagged that the psnr_hvs GPU
  round-trip scales 9- and 11-bit input by 16 in the kernel while the host
  divides by a different factor; a separate defect on an unusual bit depth,
  tracked separately.

## References

- `core/src/feature/cuda/integer_psnr_cuda.c::psnr_cuda_dispatch` (the
  kernelParams array), `core/src/feature/cuda/integer_psnr/psnr_score.cu`.
- [ADR-1212](1212-gpu-moment-bit-depth-normalisation.md) — the sibling
  bit-depth blind spot in `float_moment`, found by the same sweep.
- Source: `req` — user direction to fix bugs found by the twin-drift sweep.
