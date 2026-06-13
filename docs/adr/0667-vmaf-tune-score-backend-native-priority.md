<!-- markdownlint-disable MD013 MD060 -->
# ADR-0667: vmaf-tune score backend native priority

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris
- **Tags**: vmaf-tune, gpu, cuda, sycl, hip, vulkan, fork-local

## Context

`vmaf-tune --score-backend auto` is the score-axis accelerator for corpus,
compare, ladder, per-shot, and fast verification workflows. The selector still
treated Vulkan as the first non-CUDA fallback and did not expose HIP, even
though the libvmaf CLI now accepts `--backend hip` and the fork's AMD ROCm path
is end-to-end usable on ROCm hosts.

That mismatch is user-visible in two ways: AMD machines cannot ask
`vmaf-tune` for HIP scoring directly, and mixed-backend hosts may choose the
cross-vendor Vulkan path before the vendor-native SYCL or HIP path. The product
goal for tune is a fast consumer Netflix-style pipeline, so the default needs
to prefer the native runtime where one exists and keep Vulkan as the broad
fallback.

## Decision

`vmaf-tune --score-backend` will accept `hip` and `auto` will use
`cuda -> sycl -> hip -> vulkan -> cpu`. HIP availability is probed with
`rocminfo` first and `rocm-smi --showproductname` as a fallback, intersected
with the `vmaf --help` backend alternation the same way CUDA, SYCL, and Vulkan
are already handled.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep `cuda -> vulkan -> sycl -> cpu` and leave HIP out | No code churn; preserves earlier ADR-0314 Vulkan quick-win behaviour | Blocks AMD ROCm users from the tune score-backend surface and can choose a translation / cross-vendor stack over a native runtime | Rejected: stale relative to the current libvmaf CLI and the native-first operator rule |
| Add HIP but keep Vulkan before SYCL/HIP | Minimal change to accepted values | Still violates native-first selection on Intel and AMD hosts | Rejected: fixes only the missing explicit value, not the default speed path |
| Add both HIP and Metal in the same PR | Full parity with the libvmaf CLI enum | Metal probing is macOS-specific and requires a separate runtime decision; this PR is about the AMD/SYCL/Vulkan order used on the current consumer Linux path | Rejected for scope; Metal can be a focused follow-up |

## Consequences

- **Positive**: `vmaf-tune` can now drive `vmaf --backend hip` directly and
  auto-selection prefers native GPU stacks before Vulkan.
- **Positive**: docs and tests make the score-backend order explicit, reducing
  future drift between tune and libvmaf CLI backend support.
- **Negative**: HIP detection now depends on ROCm command-line tooling; hosts
  with a working custom HIP runtime but neither `rocminfo` nor `rocm-smi` still
  need explicit operator setup before `auto` will find HIP.
- **Neutral / follow-ups**: native Metal score-backend detection remains a
  separate macOS-specific task.

## References

- [ADR-0299](0299-vmaf-tune-gpu-score.md) — initial `vmaf-tune` GPU score backend.
- [ADR-0314](0314-vmaf-tune-score-backend-vulkan.md) — Vulkan score backend.
- [ADR-0422](0422-cli-hip-metal-backend-selectors.md) — libvmaf CLI HIP / Metal selectors.
- [docs/backends/hip/overview.md](../backends/hip/overview.md) — HIP runtime status.
- Source: `req` — "lol? hip is implemented? why avoid?"
- Source: `req` — "but always cuda > sycl > hipm/rocm > vulkan? so native first"
