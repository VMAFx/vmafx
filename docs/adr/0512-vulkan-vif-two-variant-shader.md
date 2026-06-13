<!-- markdownlint-disable MD060 -->
# ADR-0512: Vulkan VIF Two-Variant Compute Shader (fp32 Auto-Fallback)

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `vulkan`, `vif`, `gpu-parity`, `precision`, `compatibility`

## Context

[ADR-0492](0492-vulkan-vif-shader-fp64-g-computation.md) promoted the
Vulkan VIF `g` / `sv_sq` / `gg_sigma` accumulators from `precise float`
to `double` to close a ~7 ULP/px divergence from the CPU reference and
pass the ADR-0214 places=4 parity gate on RTX 4090. As part of that
change the backend init was made to *refuse* to attach on devices that
do not advertise `VkPhysicalDeviceFeatures::shaderFloat64`, falling back
to CPU with a `-ENOTSUP` diagnostic.

That hard refusal was a precaution that was never empirically
validated. On the Netflix golden 576x324 corpus
(`src01_hrc00 <-> src01_hrc01`, yuv420p 8-bit) we measure:

| Path                                     | VMAF       | Delta vs CPU |
|------------------------------------------|------------|--------------|
| CPU                                      | 76.66783   | -            |
| Vulkan fp64 (RTX 4090, shaderFloat64)    | 76.66776   | -7e-5        |
| Vulkan fp32 (Intel Arc A380, no fp64)    | 76.66775   | -8e-5        |
| Vulkan fp32 (AMD Radeon gfx1036, no fp64)| 76.66774   | -9e-5        |

The fp32 path lands within 2e-5 of the fp64 path and within ~1e-4 of
CPU — well inside the cross-backend tolerance documented in
`docs/backends/vulkan/overview.md`. The metric is identical; only the
accumulation precision differs. Excluding entire GPU generations
(Intel Arc, AMD iGPU, older NVIDIA) for an unmeasured precision
concern is not justifiable when a working fp32 path exists.

## Decision

We will ship the VIF compute shader as **two SPIR-V variants** —
`vif_fp64.comp` (the existing double-precision path) and
`vif_fp32.comp` (new, `float` for the `g`/`sv_sq`/`gg_sigma`
accumulators, `precise` qualifier to block FMA contraction). Both are
embedded into `libvmaf.so` at build time. The Vulkan backend probes
`VkPhysicalDeviceFeatures::shaderFloat64` during context init, stores
the bit on `VmafVulkanContext::has_float64`, and the VIF pipeline-create
call binds whichever variant matches the device. No user opt-in is
required for the auto-fallback.

For bit-exact-strict workflows (CI parity gates that need to assert
the fp64 path is taken) we add the inverse opt-in
`--vulkan-require-fp64` (CLI) /
`VmafVulkanConfiguration::require_fp64` (public C API). When set, the
backend reverts to the old ADR-0492 refusal behaviour on devices
without `shaderFloat64`. Default is off — most callers get the
auto-fallback transparently.

ADR-0492 retains its body unchanged per the ADR-maintenance rule
(immutable once Accepted) and gets `Status: Superseded by ADR-0509`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| A: strict-no-opt-in (status quo / ADR-0492) | Bit-exact CPU parity guaranteed; one shader to ship | Excludes Intel Arc, AMD iGPU, older NVIDIA — entire GPU generations refused for an unvalidated precision concern | Rejected — the empirical fp32 vs CPU delta (~1e-4) is well within tolerance |
| B: opt-in-flag (user must pass `--vulkan-allow-fp32`) | Conservative default; users acknowledge the precision trade | Same exclusion-by-default as A; surface friction; users on Arc / AMD iGPU hit `-ENOTSUP` first and have to discover the flag | Rejected — most users have no reason to care about ~1e-4 VMAF delta |
| C: auto-relax-flag (single shader, runtime guard relaxed) | Minimal code change | Cannot avoid the fp32-vs-double precision difference on devices without `shaderFloat64`; the shader still requires the float64 extension and would fail at SPIR-V compile time | Rejected — does not actually solve the compatibility problem |
| **D (chosen): two-variant-compile** | Both precision paths available; runtime auto-pick is transparent; bit-exact-strict workflows can still pin fp64 via opt-in | Ship two SPIR-V blobs (~2x VIF binary size); two shader sources to maintain | Best correctness / compatibility / surface trade-off |

## Consequences

- **Positive**: Vulkan backend usable on Intel Arc / AMD integrated /
  older NVIDIA GPUs. No CLI flag required for the common case.
  Bit-exact-strict CI lanes preserve old behaviour via
  `--vulkan-require-fp64`.
- **Negative**: Two shader sources to keep in lockstep (any future
  change to the integer-VIF inner loop must touch both `vif_fp64.comp`
  and `vif_fp32.comp`). Slight increase in libvmaf binary size from
  embedding both SPIR-V blobs (a few KiB per variant).
- **Neutral / follow-ups**:
  - `docs/backends/vulkan/overview.md` updated to document the
    auto-pick and the `--vulkan-require-fp64` opt-in (this PR).
  - `docs/state.md` row updated under Open / Recently closed
    (this PR).
  - `changelog.d/fixed/vulkan-fp32-fallback.md` fragment added
    (this PR).
  - AGENTS.md note in `core/src/feature/vulkan/AGENTS.md`
    records the two-variant invariant (this PR).
  - Existing tests cover the fp64 path on shaderFloat64-capable
    devices; this PR adds a fp32-fallback smoke and an
    `--vulkan-require-fp64` strict-refusal smoke.

## References

- ADR-0492: original double-precision promotion + hard refusal
  (superseded by this ADR).
- ADR-0214: GPU-parity CI gate (places=4 per-frame threshold).
- ADR-0350: `shaderBufferInt64Atomics` probe precedent (same pattern).
- research-0053: NVIDIA FMA contraction investigation (fp32 `precise`
  fix that the fp32 variant re-uses).
- Source: user direction (paraphrased) — replace the hard
  `shaderFloat64` refusal with a two-variant compile and runtime
  auto-pick; default is auto-fallback to fp32, opt-in
  `--vulkan-require-fp64` for bit-exact-strict workflows. Empirical
  fp32-vs-fp64-vs-CPU deltas (Intel Arc A380, AMD gfx1036, RTX 4090)
  collected on the dev host pre-fix and documented in the Context
  table above.
