# docs: scrub residual ANSNR references following PR #38 CPU-implementation removal

Audited all documentation for stale references to `float_ansnr`,
`float_ansnr_hip`, `float_ansnr_cuda`, `float_ansnr_metal`, and related
counts/claims that were not updated when the CPU implementation and its
GPU twins were removed in commit 70ed8b3ce3 (PR #38).

Files updated: `docs/api/gpu.md`, `docs/backends/hip/overview.md`,
`docs/backends/index.md`, `docs/backends/arm/overview.md`,
`docs/backends/metal/index.md`, `docs/development/build-flags.md`,
`docs/development/cross-backend-gate.md`, `docs/metrics/features.md`,
`docs/mcp/tools.md`, `README.md`, `core/src/feature/metal/AGENTS.md`,
`core/src/hip/AGENTS.md`, `core/src/feature/cuda/AGENTS.md`, `AGENTS.md`.

The Vulkan kernel (`float_ansnr_vulkan.c`) remains in tree as historical
dead code; the Vulkan backend itself was removed in ADR-0726.
