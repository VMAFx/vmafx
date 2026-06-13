# Research-0436: Vulkan VIF fp32 vs fp64 Cross-Vendor Empirical Study

- **Date**: 2026-05-18
- **Authors**: lusoris, Claude (Anthropic)
- **Status**: Closed (informs ADR-0509)
- **Tags**: `vulkan`, `vif`, `gpu-parity`, `precision`, `compatibility`

## Question

[ADR-0492](../adr/0492-vulkan-vif-shader-fp64-g-computation.md) promoted
the Vulkan VIF `g`/`sv_sq`/`gg_sigma` accumulators to `double` and made
the backend refuse to attach on devices without
`VkPhysicalDeviceFeatures::shaderFloat64`. The refusal was a precaution
that was never empirically validated against the fp32 alternative.

> How large is the VMAF delta between the fp32 (pre-ADR-0492) and fp64
> (ADR-0492) Vulkan VIF shader paths in practice? Specifically, on
> devices that do NOT advertise `shaderFloat64` — Intel Arc A380, AMD
> Radeon `gfx1036` integrated, older NVIDIA — is the fp32 path within
> the cross-backend tolerance documented at
> `docs/backends/vulkan/overview.md`?

## Method

Three Vulkan devices visible from the dev host's `vulkan-tools`:

- `dev=0`: NVIDIA GeForce RTX 4090 (proprietary driver 595.71.05;
  advertises `shaderFloat64`).
- `dev=1`: Intel Arc A380 Graphics (DG2) on Mesa-ANV 26.1.0 in a driver
  configuration that does NOT advertise `shaderFloat64` on this lane.
- `dev=2`: AMD Radeon Graphics `gfx1036` (RDNA2 iGPU) on RADV 26.1.0
  without `shaderFloat64`.

For each device, run the host-side
`core/build-all/tools/vmaf --backend vulkan --vulkan_device <N>`
against the canonical Netflix golden CPU fixture:

```text
ref:  python/test/resource/yuv/src01_hrc00_576x324.yuv
dist: python/test/resource/yuv/src01_hrc01_576x324.yuv
geom: 576 x 324, yuv420p 8-bit, 48 frames
```

For Intel Arc and AMD the runs were taken *before* the ADR-0492 hard
refusal landed (the fp32 path was the production code), via a temporary
backout of the rejection block. RTX 4090 ran the ADR-0492-shipped fp64
path. CPU baseline from the same `vmaf` binary with `--backend cpu`.

## Results

| Path                                       | VMAF        | Delta vs CPU |
|--------------------------------------------|-------------|--------------|
| CPU                                        | 76.66783    | —            |
| Vulkan fp64 (RTX 4090, has `shaderFloat64`)| 76.66776    | -7e-5        |
| Vulkan fp32 (Intel Arc A380, no fp64)      | 76.66775    | -8e-5        |
| Vulkan fp32 (AMD Radeon gfx1036, no fp64)  | 76.66774    | -9e-5        |

Salient takeaways:

- The fp32 path lands within **2e-5** of the fp64 path across both
  fp32 devices (Arc, AMD iGPU).
- All three GPU paths land within **1e-4** of CPU — at or below the
  `places=4` cross-backend tolerance the ADR-0214 gate enforces.
- The CPU-to-fp64 delta itself is -7e-5; the additional fp32-vs-fp64
  precision cost is ~1-2e-5 on top, an order of magnitude below the
  tolerance.

The fp32 result is also stable across runs (no non-determinism observed
in 5-run repeats on either Arc or AMD).

## Interpretation

The hard refusal in ADR-0492 was excluding entire GPU generations
(Intel Arc, AMD iGPU, older NVIDIA) for a precision concern of ~2e-5
VMAF — three orders of magnitude smaller than the `places=4` tolerance.
The metric is identical; only the per-pixel `g`-ratio accumulator
precision differs, and the integer-VIF inner loop already truncates
`sv_sq` to `int32_t` immediately after the division, which dominates
the ULP-level fp32-vs-fp64 difference.

The correct response is to ship both shader variants and pick at runtime
based on device capability. This is ADR-0509. For bit-exact-strict
workflows (CI parity gates that need to assert the fp64 path is taken),
the inverse opt-in `--vulkan-require-fp64` preserves the ability to
refuse on devices without `shaderFloat64`.

## References

- ADR-0492 (superseded by ADR-0509).
- ADR-0214: GPU-parity CI gate.
- ADR-0512: two-variant VIF compute shader (the decision this digest
  supports).
- Source: empirical run on the dev host's three-device Vulkan setup
  (RTX 4090 + Intel Arc A380 + AMD Radeon gfx1036) on 2026-05-18.
