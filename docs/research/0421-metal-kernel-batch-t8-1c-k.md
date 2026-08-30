<!-- markdownlint-disable MD013 MD060 -->
# Metal kernel batch T8-1c through T8-1k — reduction strategy and numerical stability

> ADR-0421 is the authoritative decision record. This digest summarises
> the empirical findings and numerical analysis that informed the kernel
> design choices for the full T8-1 kernel batch.

## Feature scope

Eight feature-extractor kernels written in Metal Shading Language (MSL)
for the Apple Silicon native Metal backend (T8-1 / ADR-0361 + ADR-0420).

| ID     | Kernel file                    | Feature(s)                                                   |
|--------|--------------------------------|--------------------------------------------------------------|
| T8-1c  | `integer_motion_v2.metal`      | `motion_v2_sad`, `motion2_v2` (TEMPORAL)                     |
| T8-1d  | `float_psnr.metal`             | `float_psnr`                                                 |
| T8-1e  | `float_moment.metal`           | `float_moment_ref1st/dis1st/ref2nd/dis2nd`                   |
| T8-1f  | `float_ansnr.metal`            | `float_ansnr`                                                |
| T8-1g  | `integer_psnr.metal`           | `psnr_y`, `psnr_cb`, `psnr_cr`                               |
| T8-1h  | `float_motion.metal`           | `float_motion` (TEMPORAL, 5-tap blur)                        |
| T8-1i  | `integer_motion.metal`         | `motion_y`, `motion2`, `motion3` (TEMPORAL)                  |
| T8-1j  | `float_ssim.metal`             | `float_ssim`, `float_ms_ssim` (two-pass)                     |

## Reduction strategy: per-WG partials instead of atomics

### Problem discovered

MSL's `atomic_fetch_add_explicit` for the `ulong` type (64-bit unsigned
long) compiles without error on macOS but fails silently on device:
partial sums are dropped, producing garbage scores. Confirmed in CI run
25685703780 / job 75408804495 during T8-1c bring-up.

ADR-0421 initially described `atomic_ulong` accumulation (matching the
CUDA twin's pattern). The `.metal` implementation diverges from that
description and uses per-WG `float`/`uint` partial arrays instead.

### Chosen approach: per-WG partials array

```metal
// One float partial per threadgroup
device float *partials [[buffer(2)]]
...
threadgroup float simd_partials[8];
const float lane_sum = simd_sum(my_val);
if (simd_lane == 0) simd_partials[simd_id] = lane_sum;
threadgroup_barrier(mem_flags::mem_threadgroup);
if (lid == 0) {
    float sum = 0.0f;
    for (uint i = 0; i < simd_count; ++i) sum += simd_partials[i];
    partials[bid.y * grid_groups.x + bid.x] = sum;
}
```

Host reduces the `grid_w × grid_h` float partials in `double`.

### Alternative reduction strategies considered

| Strategy | Result |
|---|---|
| Global `atomic_ulong` (CUDA pattern) | Silent failure on Apple Silicon — `atomic_fetch_add_explicit` for 64-bit silently discards updates |
| Per-WG `atomic_uint` | Works but 32-bit overflow risk for large frames (1920×1080 = 2M pixels, max per-pixel value for motion is large; float avoids this) |
| Per-SIMD `simd_sum` + one `atomic_uint` per WG | Viable but per-WG partials is simpler and avoids any atomic |
| **Per-WG partials (chosen)** | Correct, no atomics, minimal memory traffic, host double-precision reduction |

## Numerical stability analysis

### Float precision for partial sums

All kernels use `float` partials. With 16×16 = 256 threads per threadgroup
and a maximum partial value of ~255^2 ≈ 65025 per pixel, the worst-case
per-WG sum is ~16.7M. A 32-bit float has 7 decimal digits of precision.
At 16.7M the ULP is ~1.0, meaning each individual pixel may accumulate
with ±1 ULP error. For a 1920×1080 frame with ~8100 threadgroups, the
host double accumulation of 8100 float partials introduces at most ~0.01%
error, well within the `places=3` tolerance for floating-point kernels.

### Integer kernels (integer_psnr, integer_motion_v2, integer_motion)

These use `uint` partials (not float), which avoids floating-point error
at accumulation time. Host reduces in `double`. Max per-pixel `uint`
value: `(255*5*FILTER_MAX)^2 / threadgroup` — fits in uint32 for 16×16 WG.

### ansnr and float_motion convolution

Both apply 3×3/5×5 and 5-tap Gaussian convolutions respectively in
`float`. The convolution itself introduces ~1 ULP per multiply-add.
After host reduction, `places=3` is achievable (consistent with ADR-0192
tolerance for convolution-based kernels on all other GPU backends).

## Mirror padding variants

| Kernel             | Mirror type            | Formula for `idx >= sup`        |
|--------------------|------------------------|---------------------------------|
| `integer_motion_v2`| Edge-replicating       | `2*sup - idx - 1`               |
| `float_ansnr`      | Edge-replicating       | `2*sup - idx - 1`               |
| `float_motion`     | Skip-boundary          | `2*(sup-1) - idx`               |
| `integer_motion`   | Skip-boundary (motion1)| `2*(sup-1) - idx`               |

The difference matches the CPU references exactly — `motion_v2` and
`ansnr` use edge-replicating; `motion` and `motion_v2`'s v1 precursor
use skip-boundary. See ADR-0193 for the motion_v2 bring-up note.

## References

- [ADR-0421](../adr/0421-metal-first-kernel-motion-v2.md) — T8-1c spec (covers T8-1c–k)
- [ADR-0214](../adr/0214-gpu-parity-ci-gate.md) — `places=4` parity gate
- [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md) — `places=3` tolerance for convolution kernels
- CI run 25685703780 / job 75408804495 — `atomic_ulong` failure discovery
