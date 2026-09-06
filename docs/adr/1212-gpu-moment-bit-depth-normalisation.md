<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-1212: The GPU `float_moment` twins normalise by the bit-depth scaler on the host

- **Status**: Proposed
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, sycl, hip, correctness, feature-extractor, bit-depth

## Context

`float_moment` on CUDA, SYCL and HIP reported wildly wrong values for any input
above 8 bits per channel. Measured on `src01_hrc00/hrc01_576x324.yuv420p10le`,
frame 0, before this change:

| | `float_moment_ref1st` | `float_moment_ref2nd` |
|---|---|---|
| CPU `float_moment` | 61.928749785665296 | 4935.612488211591 |
| CUDA `float_moment_cuda` | 247.71499914266118 | 78969.79981138546 |

That is exactly 4x and 16x. The CPU reference (`core/src/feature/float_moment.c`)
runs `picture_copy()` before it accumulates anything, and `picture_copy()`
divides every high-bit-depth sample by a scaler — 4 at 10 bpc, 16 at 12 bpc,
256 at 16 bpc (`core/src/feature/picture_copy.cpp`) — so `moment.c` sums
normalised floats. The three GPU twins accumulate the **raw codeword** in exact
integer sums and their host `collect` paths then divide only by the pixel
count. Metal was the sole conforming twin. The HIP kernel even said so in its
own comment: "Values are accumulated raw (no normalisation)".

No parity test could see this: every fixture in `core/test/` is 8-bit, where
the scaler is 1. Found by the twin-drift sweep and upheld by three independent
adversarial verifiers (reachability, arithmetic, not-already-fixed) before any
code was touched.

## Decision

We will divide the device sums by the bit-depth scaler on the host — first
moments by `scaler`, second moments by `scaler^2` — in the CUDA, SYCL and HIP
`collect` paths, using the same 10/12/16 → 4/16/256 mapping as
`picture_copy()`. The kernels are untouched.

This is not an approximation. Every `x / scaler` the CPU sums is an exact
multiple of `1/scaler`, the running `double` sum stays exact, and the device
integer sums are exact, so `(sum x) / scaler` reproduces the CPU
**bit-for-bit** at 10 and 12 bpc. At 16 bpc the CPU rounds each `float`
square before accumulating, so agreement there is to float precision; the
places=4 gate absorbs it. 8-bit results do not change (`scaler = 1`).

The parity fixtures are made bit-depth generic (`FIXTURE_BPC > 8` writes
`uint16` samples with an independent pattern in the low bits) and each
`float_moment` parity TU is registered a second time at 10 bpc. HIP, which had
no `float_moment` gate at all, gets one.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Divide the exact integer sums on the host (chosen) | Bit-identical to the CPU at 10/12 bpc; no kernel change; three small host edits | 16 bpc agrees only to float precision | — |
| Normalise inside the kernels (float accumulation of `x / scaler`) | Mirrors the CPU's data flow literally | Loses the exact-integer accumulator, introduces GPU float summation-order drift at every bit depth, and touches four kernel sources for no accuracy gain | Rejected |
| Stage through `picture_copy()` on the host and upload floats | Reuses the reference code path verbatim | Doubles the upload bandwidth and per-frame host work for a two-line arithmetic fix | Rejected |
| Reject `bpc != 8` in the GPU twins' `init` | Loud instead of wrong | Removes a shipped surface (`--feature float_moment_cuda` on 10-bit input) rather than fixing it | Rejected |

## Consequences

- **Positive**: CUDA, SYCL and HIP `float_moment` now equal the CPU to the
  printed 9 significant digits at 8, 10 and 12 bpc on the Netflix 576x324 pair
  (RTX 4090, Arc A380, gfx1030). The new 10-bit gates pass on all three.
- **Negative**: any downstream artifact that consumed a GPU `float_moment` at
  >8 bpc was consuming a value 4x–256x too large. No fork-added snapshot does.
- **Neutral / follow-ups**: the Metal twin accumulates its per-tile partials in
  `float32` where the CPU uses `double`; at ≥10 bpc and large frames the tile
  sums leave float32's exact range. Not addressed here — no Apple hardware to
  verify against — and tracked separately.

## References

- CPU reference: `core/src/feature/float_moment.c`, `core/src/feature/picture_copy.cpp`, `core/src/feature/moment.c`.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — the places=4 cross-backend gate.
- [ADR-1206](1206-gpu-parity-large-fixture-variants.md) — the sibling blind
  spot (one fixture *resolution*); this ADR closes the one-fixture *bit depth*.
- Reproducer: `vmaf -r src01_hrc00_576x324.yuv420p10le.yuv -d src01_hrc01_576x324.yuv420p10le.yuv --width 576 --height 324 --pixel_format 420 --bitdepth 10 --no_prediction --feature float_moment_cuda --output /dev/stdout --json`; compare against `--feature float_moment`.
- Source: `req` — user direction to fix bugs found by the twin-drift sweep.
