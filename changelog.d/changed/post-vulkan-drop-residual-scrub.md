- **docs:** Post-ADR-0726 residual scrub — remove stale Vulkan references from
  `docs/ai/datasets/k150k.md` (`--no_vulkan` flag drop), `docs/mcp/tools.md`
  (run_benchmark backend list), `docs/api/index.md` (Vulkan header row), and
  `docs/api/gpu.md` (Vulkan section status + `-Denable_vulkan=true` invalid
  Meson syntax). Backends/index.md, vulkan/overview.md, and
  context-api-contract.md were already correct post-#607.
