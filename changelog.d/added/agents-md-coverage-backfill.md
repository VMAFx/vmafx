- **AGENTS.md per-package coverage audit + backfill.**
  Audits every code-bearing directory under `core/src/`, `tools/`,
  `ai/`, `python/vmaf/`, `mcp-server/`, and `scripts/` for the
  rebase-sensitive-invariants documentation required by CLAUDE.md
  §12 r11 / [ADR-0108](../docs/adr/0108-deep-dive-deliverables-rule.md).
  Adds 13 new `AGENTS.md` files where rebase risk was real but
  documentation was missing: the SIMD twin-update tables under
  `core/src/feature/{x86,arm64}/`, the IQA scalar reference
  (`core/src/feature/iqa/`) and Xiph third-party reference
  (`core/src/feature/third_party/xiph/`), the per-feature GPU
  kernel directories (`core/src/feature/{cuda,sycl,vulkan}/` plus
  `core/src/feature/vulkan/shaders/`), the SVE2 HWCAP2 fork-local
  fallback under `core/src/arm/`, the MCP scaffold contract
  (`core/src/mcp/`), the fork-original ensemble training kit
  (`tools/ensemble-training-kit/`), and the top-level `scripts/`
  tree (covers ADR-0221 fragment-concat scripts, ONNX placeholder
  generators, setup dispatcher). Each new file documents its
  package-specific invariants — twin-update rules, upstream-mirror
  discipline, and ADR carve-outs — so a contributor opening any of
  those directories cold finds the rebase-sensitive context without
  reading the parent end-to-end. Audit summary lives at
  [`docs/research/0090-agents-md-coverage-audit-2026-05-09.md`](../docs/research/0090-agents-md-coverage-audit-2026-05-09.md).
  No engine or test changes; documentation-only.
