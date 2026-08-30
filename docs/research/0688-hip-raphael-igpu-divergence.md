<!-- markdownlint-disable MD013 MD060 -->
# Research-0688: HIP AMD Raphael iGPU divergence — carry-loss in int64 wavefront reduction

**Date**: 2026-05-28
**Scope**: Root-cause analysis of 19-point VMAF HIP divergence on AMD gfx1036 (Raphael
iGPU, `HSA_OVERRIDE_GFX_VERSION=10.3.0`) vs CPU/CUDA/SYCL/Vulkan.
**Outcome**: Two stacked bugs found and fixed. See [ADR-0688](../adr/0688-hip-wave32-vif-motion-fix.md).

---

## 1. Trigger

PR #1561 added a dev-container smoke test that ran `vmaf --backend hip` against the
Netflix golden `src01_hrc00_576x324 ↔ src01_hrc01_576x324` pair.  The result was
**57.335** while all other backends scored **76.668** — a 19.333-point divergence.

---

## 2. Environment

| Item | Value |
|------|-------|
| Host GPU | AMD Radeon 680M (gfx1036, Raphael APU integrated) |
| ROCm | 7.2.3 |
| `HSA_OVERRIDE_GFX_VERSION` | `10.3.0` (maps gfx1036 → gfx1030 code path) |
| `warpSize` at runtime | 32 (RDNA2/RDNA3 wave32 default) |
| Container | `vmaf-dev-mcp` |

---

## 3. Investigation chain

### Step 1 — Feature-level decomposition

Running HIP vs CPU per feature on the Netflix src01 pair yielded:

| Feature | CPU | HIP (broken) | Delta |
|---------|-----|--------------|-------|
| `adm2` | 0.934506 | 0.934506 | 0.000000 |
| `motion2` | 0.717396 | 0.000000 | -0.717396 |
| `vif_scale0` | 0.505714 | 0.000012 | -0.505702 |
| `vif_scale1` | 0.879122 | 0.000014 | -0.879108 |
| `vif_scale2` | 0.937640 | 0.000015 | -0.937625 |
| `vif_scale3` | 0.963449 | 0.000016 | -0.963433 |
| `VMAF` | 76.668 | 57.335 | -19.333 |

ADM was exact; VIF and motion were essentially zero.

### Step 2 — Motion kernel (sub-fix 1, warpSize)

`motion_score.hip` had hardcoded `MS_WARP_SIZE=64` for warp-stride and lane-0
detection.  On RDNA2/3 the wavefront is 32 lanes.  With stride 32 in the reduction
loop, `__shfl_down(x, 32)` on a 32-lane wavefront reads lane `(lid + 32) % 32`
(OOB by warp; HIP silently wraps or returns 0), producing a zero accumulator.

Fix: replace `MS_WARP_SIZE` with runtime `warpSize`.  After this fix, motion delta
= 0.000000.

### Step 3 — VIF kernel (sub-fix 2, carry-preserving int64 reduction)

After the warpSize fix, VIF scales were still near zero.  Debugging via a
temporary `fprintf(stderr, ...)` in `write_scores_hip()` dumped the raw accumulator:

```text
[HIP-VIF-DEBUG frame=0] x = -776495728965065  (expected ~-1586633)
```

This is five orders of magnitude too large in absolute value.  The field `x`
accumulates `get_best16_from32` shift amounts (typically -2 to -16 per pixel).
For 576×324 = 186 624 pixels at scale-0, a reasonable per-pixel mean of -8.5 gives
approximately -1.59 million — consistent with the expected value.

The actual value, -776 trillion, is consistent with the carry-loss hypothesis.

### Step 4 — Carry-loss proof

`wavefront_reduce_i64` in `vif_statistics.hip` split `int64_t x` into independent
32-bit halves and reduced them with separate `__shfl_xor` passes:

```c
uint32_t lo = (uint32_t)(x & 0xffffffff);
uint32_t hi = (uint32_t)((uint64_t)x >> 32);
for (unsigned stride = warpSize / 2u; stride > 0u; stride >>= 1u) {
    lo += __shfl_xor(lo, stride);
    hi += __shfl_xor(hi, stride);
}
return (int64_t)((uint64_t)lo | ((uint64_t)hi << 32));
```

For a single warp where all 32 lanes carry `x = -2`:

- `lo = 0xFFFFFFFE`, `hi = 0xFFFFFFFF`
- After XOR-tree: `lo_sum = (32 × 0xFFFFFFFE) mod 2^32`

  `32 × 0xFFFFFFFE = 32 × (2^32 - 2) = 32 × 2^32 - 64`

  `mod 2^32 = -64 mod 2^32 = 0xFFFFFFC0`

  Each `lo += __shfl_xor(...)` step: e.g. adding two copies of `0xFFFFFFFE` → `0xFFFFFFFC` (carry out of bit 31, lost). After 5 doublings, the carry counts 31 lost carry-bits = 31 × 2^32 ≈ 133 billion error.

- After XOR-tree: `hi_sum = (32 × 0xFFFFFFFF) mod 2^32 = 0xFFFFFFE0`

  (loses 31 carry bits = 31 units of 2^32 from the `lo` accumulation **not** counted here)

- Assembled result: `(0xFFFFFFE0 << 32) | 0xFFFFFFC0 = 0xFFFFFFE0FFFFFFC0`

  As int64: `-133 143 986 240` — wrong by +133 143 986 176 (i.e., the 31 lost carries × 2^32 = 31 × 4 294 967 296 ≈ 133 billion).

- Correct result: `32 × (-2) = -64 = 0xFFFFFFFFFFFFFFC0`.

For the full 576×324-pixel VIF kernel with per-pixel values around -8 to -16, the
accumulated carry loss reaches -776 trillion, confirmed by the debug output.

### Step 5 — Reference: CUDA carry-preserving pattern

`libvmaf/src/cuda/cuda_helper.cuh` lines 119–127:

```c
__forceinline__ __device__ int64_t warp_reduce(int64_t x)
{
    for (int i = 16; i > 0; i >>= 1) {
        x += int64_t(__shfl_down_sync(0xffffffff, x & 0xffffffff, i)) |
             int64_t(__shfl_down_sync(0xffffffff, x >> 32, i) << 32);
    }
    return x;
}
```

Key insight: the neighbour lane's `lo` and `hi` words are OR-ed into a 64-bit value
**before** adding to `x`.  This means the 64-bit addition's carry propagates
naturally from bit 31 to bit 32.  The HIP fix mirrors this pattern using
`__shfl_xor` (HIP lacks `__shfl_down_sync`):

```c
for (unsigned stride = warpSize / 2u; stride > 0u; stride >>= 1u) {
    int64_t neighbour =
        (int64_t)((uint64_t)(unsigned)__shfl_xor((int)(x & 0xffffffff), (int)stride) |
                  ((uint64_t)(unsigned)__shfl_xor((int)((uint64_t)x >> 32), (int)stride) << 32));
    x += neighbour;
}
```

### Step 6 — Boundary experiment (negative result)

To investigate whether the residual 0.003–0.014 per-scale VIF delta (after carry fix)
was from boundary treatment, `clamp_i` (replicate) was replaced with `reflect101_i`
(skip-boundary mirror, matching the CUDA SMEM tile approach).  This made the delta
worse (0.003 → 0.007 per scale), confirming the residual is from `log2f()` hardware
float precision, not boundary choice.  The boundary was reverted to `clamp_i`.

---

## 4. Results after fix

| Feature | CPU | HIP (fixed) | Delta |
|---------|-----|-------------|-------|
| `motion_score` | 0.808... | 0.808... | 0.000000 |
| `motion2_score` | 0.717... | 0.717... | 0.000000 |
| `vif_scale0` | 0.505714 | ~0.503 | ~0.003 |
| `vif_scale1` | 0.879122 | ~0.876 | ~0.003 |
| `vif_scale2` | 0.937640 | ~0.936 | ~0.002 |
| `vif_scale3` | 0.963449 | ~0.962 | ~0.001 |
| `VMAF` pooled mean | 76.667830 | 76.439537 | 0.228 |

The 19.333-point divergence is reduced to 0.228 (98.8% recovered).  Motion is
bit-exact.  The residual 0.228 VIF-driven VMAF delta is from `log_generate()` using
`log2f()` hardware float (24-bit mantissa) vs the CPU's precomputed integer LUT.

---

## 5. Root cause summary

| Bug | Location | Effect |
|-----|----------|--------|
| Hardcoded `warpSize=64` | `motion_score.hip` | `__shfl_down(x, 32)` OOB on wave32 → zero SAD accumulator → motion=0 |
| Split lo/hi int64 reduction (carry loss) | `vif_statistics.hip::wavefront_reduce_i64` | Each partial-sum carry from bit 31→32 discarded → accumulator off by ~5 orders of magnitude → VIF≈0 |

---

## 6. References

- [ADR-0688](../adr/0688-hip-wave32-vif-motion-fix.md)
- [PR #1561](https://github.com/VMAFx/vmafx/pull/1561) — original smoke-test trigger
- [ADR-0552](../adr/0552-hip-integer-vif-deterministic-reduce.md) — prior VIF reduction fix
- `libvmaf/src/cuda/cuda_helper.cuh` — CUDA carry-preserving reference
- `libvmaf/src/feature/hip/integer_vif/vif_statistics.hip` — fixed kernel
- `libvmaf/src/feature/hip/integer_motion/motion_score.hip` — fixed kernel
