<!-- markdownlint-disable MD013 MD060 -->
# ADR-0688: HIP wave32 carry-preserving int64 reduction for VIF and motion kernels

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: Lusoris
- **Tags**: hip, numerics, vif, motion, bugfix, fork-local

## Context

PR #1561 (dev-container smoke test) surfaced a critical divergence: `vmaf --backend hip`
scored **57.335** on the Netflix golden `src01_hrc00_576x324 ↔ src01_hrc01_576x324` pair
while every other backend (CPU, CUDA, SYCL, Vulkan) scored **76.668** — a 19-point delta
that completely invalidates the HIP output.

Investigation inside the `vmaf-dev-mcp` container on AMD gfx1036 (Radeon 680M,
`HSA_OVERRIDE_GFX_VERSION=10.3.0` maps to gfx1030) traced the divergence through two
stacked bugs in the HIP VIF and motion kernels.

### Sub-fix 1 — warpSize (motion, already applied in a prior session)

The original `motion_score.hip` hardcoded `MS_WARP_SIZE=64` for the inter-warp stride
in its SAD reduction.  RDNA2/RDNA3 (`gfx1030`/`gfx1036`) runs in wave32 (32 lanes per
wavefront) by default; the hardcoded 64 produced out-of-bounds lane accesses via
`__shfl_down(x, 32)` on a 32-lane wavefront, injecting garbage into the accumulator.

Fix: replace every compile-time warp-size constant with the device-side `warpSize`
variable, which returns 32 on gfx1030/gfx1036 and 64 on GCN/Vega at runtime.

### Sub-fix 2 — carry-propagating int64 reduction (VIF, primary fix)

`vif_statistics.hip`'s `wavefront_reduce_i64` split the `int64_t` accumulator into two
independent 32-bit halves and reduced them separately:

```c
/* Broken — independent lo/hi shfl loses carry from lo into hi */
uint32_t lo = (uint32_t)(x & 0xffffffff);
uint32_t hi = (uint32_t)((uint64_t)x >> 32);
for (unsigned stride = warpSize / 2u; stride > 0u; stride >>= 1u) {
    lo += __shfl_xor(lo, stride);
    hi += __shfl_xor(hi, stride);
}
return (int64_t)((uint64_t)lo | ((uint64_t)hi << 32));
```

For a wavefront where all 32 lanes carry `x = -2` (`lo = 0xFFFFFFFE`,
`hi = 0xFFFFFFFF`):

- Broken: `lo_sum = 32 × 0xFFFFFFFE mod 2^32 = 0xFFFFFFC0`,
  `hi_sum = 32 × 0xFFFFFFFF mod 2^32 = 0xFFFFFFE0`
  → result `0xFFFFFFE0FFFFFFC0 = -133 143 986 240` (wrong by factor ~2 billion).
- Correct: `32 × (-2) = -64 = 0xFFFFFFFFFFFFFFC0`.

Every carry from the `lo` partial sum was silently dropped because the carry bit
(representing +1 unit of 2^32) was never propagated into the `hi` half.

The VIF `x` accumulator holds `get_best16_from32` shift amounts (typically -2 to -16
per pixel).  With 576×324 pixels across four scales the correct sum is approximately
-1.6 million; the broken reduction produced -776 trillion — a 5-order-of-magnitude
error that collapsed all four VIF scale scores, which in turn caused the SVM to
output a score near the floor (57.335 instead of 76.668).

The CUDA twin (`cuda_helper.cuh::warp_reduce`) avoids this by keeping `x` as
`int64_t` throughout and reconstructing the neighbour lane's value by ORing two
32-bit shuffles, then performing 64-bit addition:

```c
// CUDA (carry-preserving):
for (int i = 16; i > 0; i >>= 1) {
    x += int64_t(__shfl_down_sync(0xffffffff, x & 0xffffffff, i)) |
         int64_t(__shfl_down_sync(0xffffffff, x >> 32, i) << 32);
}
```

The HIP fix applies the same pattern using `__shfl_xor` (HIP does not have
`__shfl_down_sync`; `__shfl_xor` achieves the same XOR-tree reduction):

```c
/* Fixed — carry-preserving */
for (unsigned stride = warpSize / 2u; stride > 0u; stride >>= 1u) {
    int64_t neighbour =
        (int64_t)((uint64_t)(unsigned)__shfl_xor((int)(x & 0xffffffff), (int)stride) |
                  ((uint64_t)(unsigned)__shfl_xor((int)((uint64_t)x >> 32), (int)stride) << 32));
    x += neighbour;
}
```

The two shuffles fetch the neighbour lane's `lo` and `hi` words atomically (same lane,
same instruction, same warp cycle), reassemble them into a 64-bit value before
adding, and thus preserve every carry.

## Decision

Apply both sub-fixes:

1. **`motion_score.hip`** — replace `MS_WARP_SIZE` compile-time constant with
   runtime `warpSize` everywhere (warp-stride loop, lane-0 detection).  This was
   already applied in a prior session; confirmed as sufficient for motion (delta = 0).

2. **`vif_statistics.hip`** — replace the split lo/hi `wavefront_reduce_i64` with
   the carry-preserving two-shuffle pattern.  The `warpSize` variable is also used
   for the lane-0 detection guard (`threadIdx.x % (unsigned)warpSize`).

No changes to boundary treatment (the `clamp_i` replicate-boundary helper is
retained; testing showed that switching to reflect-101 worsened the VIF delta from
0.003 to 0.007 per scale — the residual is from `log2f()` hardware-float precision,
not boundary choice).

## Residual delta

After the fix, VMAF HIP on the Netflix golden pair = **76.439537** vs CPU **76.667830**
(delta = 0.228).  This 0.3% residual comes entirely from `log_generate()` in
`vif_statistics.hip`, which computes log2 coefficients using hardware `log2f()` with
float32 precision, whereas the CPU reference uses a precomputed integer lookup table.
This is a pre-existing HIP VIF precision limitation documented in ADR-0552; it is
not introduced by this fix.  Motion metrics are exact CPU match (delta = 0.000000).

The combined VMAF delta of 0.228 is above the ADR-0214 places=4 gate (1e-4).  The
root cause (hardware log2f vs integer LUT) is tracked as a separate follow-up in
the HIP VIF precision work.  This fix closes the critical 19-point divergence that
made HIP useless in practice.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-thread `atomicAdd` into a shared int64 accumulator (same pattern as `vif_statistics.hip`'s ADR-0552 fix) | Bit-exact; no shuffle complexity | Requires shared-memory atomic traffic; slower than register reduction for small accumulators | The `x` accumulator is small enough that warp-register reduction is appropriate here; atomics are reserved for accumulation across blocks |
| Keep split lo/hi but add explicit carry propagation (`hi += (lo < prev_lo)`) | Minimal code change | Requires saving pre-addition state; still tricky to get right under XOR-tree ordering | The carry-preserving neighbour-then-add pattern is simpler and matches the CUDA reference exactly |
| Replace `log2f()` with integer LUT to close the residual 0.228 delta | Achieves places=4 parity | Larger scope change; risk of introducing new bugs in the log path | Deferred — the critical divergence fix ships first; precision parity is a follow-up |

## Consequences

- **Positive**: HIP VMAF recovers from 57.335 to 76.440 on the Netflix golden pair
  (19.3-point divergence reduced by 98.8%).  Motion metrics are exact CPU match.
  The HIP backend is now usable in practice.
- **Negative**: The residual 0.228 VMAF delta (from hardware `log2f` precision in
  the VIF kernel) remains.  This PR does not close the ADR-0214 places=4 gate for
  the full VMAF score.
- **Neutral / follow-ups**: The integer-LUT log2 path follow-up (to close the
  residual precision gap) should reference this ADR.  The research digest
  `docs/research/0688-hip-raphael-igpu-divergence.md` contains the full
  investigation trace.

## References

- req: "PR #1561 (dev-container smoke test) surfaced a CRITICAL HIP backend bug:
  VMAF scores 57.335 on Netflix golden pair with `--backend hip` when CPU/CUDA/SYCL/Vulkan
  score 76.668."
- [ADR-0552](0552-hip-integer-vif-deterministic-reduce.md) — prior VIF wavefront
  reduction fix (per-thread atomics replacing non-deterministic `__shfl_xor`);
  this ADR supersedes the wave32-related clause therein.
- [ADR-0214](0214-gpu-parity-ci-gate.md) — cross-backend parity gate (places=4).
- [Research-0688](../research/0688-hip-raphael-igpu-divergence.md) — full
  investigation trace: carry-bug proof, debug accumulator values, metric comparison
  table.
- [PR #1561](https://github.com/VMAFx/vmafx/pull/1561) — dev-container smoke test
  that first surfaced the 19-point divergence.
- `libvmaf/src/cuda/cuda_helper.cuh` — the CUDA carry-preserving `warp_reduce`
  reference implementation (lines 119–127).
