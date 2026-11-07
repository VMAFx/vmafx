<!-- markdownlint-disable MD060 -->
# Vulkan compute backend (opt-in, integer-first — ADR-3402)

> **Status: opt-in, integer-first, capability-gated per
> [ADR-3402](../../adr/3402-vulkan-integer-compute-opt-in.md) (2026-11-07),
> partially superseding [ADR-0726](../../adr/0726-drop-vulkan-backend.md).**
> The backend returns behind `-Denable_vulkan=enabled` (meson feature,
> default `disabled`) with **integer features only** — `integer_adm`
> (`adm2`) first — plus a device self-check probe. The floating-point
> shader pipeline, the CI lanes and the public `libvmaf_vulkan.h` API
> that ADR-0726 removed **stay removed**. The CLI flags
> `--vulkan_device` / `--no_vulkan` / `--backend vulkan` remain
> unimplemented; a user-facing probe flag is an open item (#3319).
> For the other GPU backends see [CUDA](../cuda/overview.md),
> [SYCL](../sycl/overview.md), [HIP](../hip/overview.md), and
> [Metal](../metal/index.md).

## What the opt-in path is

A single integer feature extractor, `integer_adm_vulkan` (registered by
name; no dispatch flag, no public ABI — the
`VMAF_PICTURE_BUFFER_TYPE_VULKAN_DEVICE` and
`VMAF_FEATURE_EXTRACTOR_VULKAN` enum slots stay reserved pending the
ABI-bump ADR), plus the probe. Frame transport is host upload/download;
zero-copy import is a later optimization.

- **Build**: `-Denable_vulkan=enabled` requires the Vulkan headers, a
  GLSL-to-SPIR-V compiler (`glslang` or `glslc`), and the `volk` +
  `Vulkan-Memory-Allocator` meson wraps (both MIT, re-added under
  `core/subprojects/`). With the option off (the default) the tree
  configures and builds exactly as without this path — no CI lane
  enables it.
- **Device hygiene** (ADR-3402, from the recorded failure classes):
  `vkCreateDevice` requests only device-reported features (the Intel
  `vkCreateDevice` SIGSEGV, #2466); devices are reported and selected by
  `vendorID`/`deviceID`, never ordinals (research-0090); a dispatch
  sentinel proves the kernel ran (the ADR-0176 errata lesson); no
  shader requires `shaderFloat64` — the angle predicate is evaluated
  through the fp64-free `adm_angle_flag_i64()` reformulation
  (research-2030), so the Intel iGPU `0x8086:0xa78b`
  (`shaderFloat64=0`) stays serviceable.
- **No subgroup int64 operations**: the known NVIDIA
  `subgroupAdd(int64_t)` lowering bug (ADR-0269) is avoided by
  construction — the kernel folds its stage-3 reduction as a
  shared-memory serial sum; the two-level reduction shaders are not
  restored.
- **Numerics**: bit-exact against the unmodified CPU extractor, under
  the ADR-3402 parity-gate contract (`%a` hex-float strings compared
  exactly, committed `refs/` + `out/` under `core/test/data/vulkan-adm/`,
  comparator self-test that refuses a planted flip). Every feature
  answers `-ENOTSUP` on device classes the gate has not passed.

## Evidence status and limits

The parity gate passed on NVIDIA Windows (RTX 5080, RTX 4060 Laptop,
driver 621.257) on both gate fixtures; `NoContraction` measured honored
with 0 violations on NVIDIA and Intel 0.101.7092 (#2466 comment
6689054122). The evidence is explicitly incomplete, not hidden:

- **AMD: zero rows** (no device on hand); **no Linux/Mesa data at all**.
  Per the ADR-3402 entry rule nothing beyond the probe is declared for
  those device classes.
- The in-tree shader drops the Netflix-style scale-0 CM own-tap
  `int16_t` wrap (ADR-1402) and evaluates the angle test fp64-free —
  both bit-identical reforms, but the committed GPU outputs predate
  them; the gate must be re-run on real hardware (the maintainers' own
  runner, including a planted flip the gate must refuse — #3302
  comment 6691339657, #3319 work item 2) before any merge decision.
- No performance claims: the PoC ran under ~98-100% SM load;
  quiet-window measurement is pending (dispatch overhead first,
  research-0135).
- The Intel fp64 DEVICE_LOST defect's code fix has landed (the i64
  angle port); the Intel re-gate is pending hardware
  (`T-VK-INTEGER-ADM-FP64-ANGLE-DEVICE-LOST-2026-11-06`).

## Historical implementation record (ADR-0726 era)

The implementation history (kernels, cross-backend gates, the
submit-pool and two-level-reduction optimisations, the fp64/fp32 VIF
auto-pick, and the buffer-classification rules) is captured in the
governing ADRs. Of the surfaces they describe, only the runtime subset
the integer path needs was restored (from snapshot `816a5f33f`):

- [ADR-0127](../../adr/0127-vulkan-compute-backend.md) — governance
  decision to add the backend.
- [ADR-0175](../../adr/0175-vulkan-backend-scaffold.md) — scaffold-only
  audit-first PR.
- [ADR-0176](../../adr/0176-vulkan-vif-cross-backend-gate.md),
  [ADR-0177](../../adr/0177-vulkan-motion-kernel.md),
  [ADR-0178](../../adr/0178-vulkan-adm-kernel.md) — the default-model
  kernels and their cross-backend gate.
- [ADR-0185](../../adr/0185-vulkan-hide-volk-symbols.md),
  [ADR-0198](../../adr/0198-volk-priv-remap-static-archive.md) —
  volk-symbol hiding for shared and static link lines.
- [ADR-0186](../../adr/0186-vulkan-image-import-impl.md) — the
  zero-copy image-import surface (NOT restored; superseded by the
  ADR-2152 import surface).
- [ADR-0726](../../adr/0726-drop-vulkan-backend.md) — the removal
  (stands for everything outside the opt-in integer path).
- [ADR-3402](../../adr/3402-vulkan-integer-compute-opt-in.md) — this
  partial revival.
