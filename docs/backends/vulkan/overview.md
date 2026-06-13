<!-- markdownlint-disable MD060 -->
# Vulkan compute backend (removed — historical reference)

> **Status: REMOVED per [ADR-0726](../../adr/0726-drop-vulkan-backend.md)
> (2026-05-28).** The Vulkan backend, all associated source files
> (`core/src/vulkan/`, `core/src/feature/vulkan/`), the public header
> (`libvmaf_vulkan.h`), and the `enable_vulkan` meson option have been deleted.
> The CLI flags `--vulkan_device`, `--no_vulkan`, and `--backend vulkan` are
> no longer accepted, and `-Denable_vulkan=enabled` is no longer a valid build
> flag. This page is preserved only as a historical pointer. For active GPU
> backends see [CUDA](../cuda/overview.md), [SYCL](../sycl/overview.md),
> [HIP](../hip/overview.md), and [Metal](../metal/index.md).

## Why it was removed

The Vulkan backend reached full default-model coverage (VIF, motion, ADM,
plus the GPU long-tail kernels) before being retired. It was removed because
its maintenance cost — a bespoke GLSL/SPIR-V kernel set, volk-symbol-hiding
machinery for static FFmpeg links, and a software-ICD (lavapipe) CI lane —
no longer justified its place alongside the CUDA, SYCL, HIP, and Metal
backends, which together cover the same hardware vendors. See
[ADR-0726](../../adr/0726-drop-vulkan-backend.md) for the full decision and
the runner-up alternatives.

## Historical implementation record

The implementation history (kernels, cross-backend gates, the
submit-pool and two-level-reduction optimisations, the fp64/fp32 VIF
auto-pick, and the buffer-classification rules) is captured in the
governing ADRs. None of the surfaces they describe exist in the current
tree:

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
  zero-copy image-import surface (consumed by the FFmpeg patches).
- [ADR-0726](../../adr/0726-drop-vulkan-backend.md) — the removal.
