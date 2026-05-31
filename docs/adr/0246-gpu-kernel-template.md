<!-- markdownlint-disable MD060 -->
# ADR-0246: Per-backend GPU kernel scaffolding templates (CUDA + Vulkan)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: Lusoris, Claude
- **Tags**: gpu, cuda, vulkan, refactor, fork-local

## Context

Every fork-added GPU feature kernel re-implements the same lifecycle
boilerplate by hand. On CUDA that's a private non-blocking stream + two
events + a device accumulator + a pinned host readback slot, with a
`cuCtxPushCurrent` / `cuCtxPopCurrent` ladder for init and a synchronise +
destroy ladder for close. On Vulkan it's a descriptor-set layout + pipeline
layout + shader module + compute pipeline + descriptor pool, plus a
per-frame command-buffer + fence pair. Both lifecycles are mechanical, but
they're hand-rolled in every kernel — a 14-kernel CUDA matrix and a
15-kernel Vulkan matrix means roughly 90 copies of the same six-line stream

- event scaffold and 30 copies of the same descriptor-pool sizing
heuristic.

A sister-agent scope analysis (referenced under "Reproducer / smoke-test
command" below) measured the savings: ~6 LOC per CUDA kernel migration,
~30 LOC per Vulkan kernel migration. The wins are real but small per
kernel; the larger value is in centralising the partial-init unwind paths
(forgetting `cuStreamSynchronize` before `cuStreamDestroy` was the root
cause of one regression; leaking a `VkDescriptorPool` on a
`vkCreateComputePipelines` failure was a second).

The same analysis ruled out a *cross-backend* template: CUDA's
async-stream + event model and Vulkan's command-buffer + fence +
descriptor-pool model share no concrete shape. A unified abstraction
would force a lowest-common-denominator API that captures neither.

## Decision

Land **per-backend** kernel scaffolding templates as header-only inline
helpers under
[`core/src/cuda/kernel_template.h`](../../core/src/cuda/kernel_template.h)
and
[`core/src/vulkan/kernel_template.h`](../../core/src/vulkan/kernel_template.h).
The templates are **template-only**: no existing kernel includes them in
this PR. Each future kernel migration ships in its own PR, gated by the
existing `places=4` cross-backend-diff lane (per
[ADR-0214](0214-gpu-parity-ci-gate.md)) plus the Netflix CPU golden gate.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-backend templates (this ADR) | Honest about the underlying platform; helpers can use the platform's idioms (events vs fences) directly; small, focused diffs | Two headers instead of one; a new GPU backend (HIP, Metal) needs its own template | Chosen — matches the actual shape of CUDA vs Vulkan code |
| Single cross-backend template | One mental model for "GPU kernel lifecycle"; new backends bolt onto the same shape | Lowest-common-denominator API drops async-stream nuance on CUDA *and* descriptor-pool nuance on Vulkan; helper bodies become switch-on-backend ladders | Sister-agent analysis showed the two backends share no concrete shape — the unified API would be a fiction |
| Macros (CUDA helper-style `BEGIN/END` pairs) | One-line use-sites; matches the existing `CHECK_CUDA_GOTO` style | cuda-gdb / Nsight / RenderDoc step poorly through macro-expanded blocks; type errors land at expansion sites, not call sites | Step-debugging GPU code is already hard; the macro form raises the floor unnecessarily |
| Helper functions (this ADR) | Debuggable; type-checked at the call site; the existing `CHECK_CUDA_*` macros stay where they pay off | Inline functions still expand into the caller — no real cost | Chosen |
| Templates + migrate-with-template (one big PR) | One PR closes the boilerplate-debt bullet | Each migration would gate on its own `places=4` cross-backend-diff cell; failures would be hard to localise; PR review surface explodes | Each migration is its own PR with a focused gate — see ADR-0214 |
| Templates only, deferred migrations (this ADR) | Templates land unused, breaking nothing; each migration is small + gated | The boilerplate-debt bullet stays open until follow-up PRs land | Chosen — splits risk and review cost |

## Consequences

- **Positive**: Future kernel migrations get a small, focused diff with a
  central place to encode the lifecycle invariants (stream sync before
  destroy, descriptor pool sized to frames-in-flight × n_planes, fence
  freed on partial-init failure). Each migration is independently
  reviewable and gated.
- **Negative**: 706 LOC of helper headers exist with zero callers in this
  PR — adopters arrive over the follow-up T-rows. New GPU backends (HIP,
  Metal) will eventually want their own template; the per-backend split
  is honest but not free.
- **Neutral / follow-ups**:
  - `T7-XX-followup-a`: SYCL kernel-template refactor (deferred — needs
    icpx host).
  - `T7-XX-followup-b`: migrate fork-added CUDA kernels to template
    (`integer_psnr_cuda` first, then `ssimulacra2_cuda`).
  - `T7-XX-followup-c`: migrate fork-added Vulkan kernels to template
    (`psnr_vulkan` first, then `motion_vulkan` / `ssim_vulkan` /
    `cambi_vulkan`).
  - User-facing doc lives at
    [`docs/backends/kernel-scaffolding.md`](../backends/kernel-scaffolding.md).
  - Rebase invariant rows added to
    [`core/src/cuda/AGENTS.md`](../../core/src/cuda/AGENTS.md) and a
    new
    [`core/src/vulkan/AGENTS.md`](../../core/src/vulkan/AGENTS.md);
    the templates are fork-local additions that an upstream sync must
    not silently drop.

## References

- Reference CUDA kernel:
  [`core/src/feature/cuda/integer_psnr_cuda.c`](../../core/src/feature/cuda/integer_psnr_cuda.c)
  (T7-23 / [ADR-0182](0182-gpu-long-tail-batch-1.md)).
- Reference Vulkan kernel:
  [`core/src/feature/vulkan/psnr_vulkan.c`](../../core/src/feature/vulkan/psnr_vulkan.c)
  (T7-23 / [ADR-0216](0216-vulkan-chroma-psnr.md)).
- Cross-backend gate that gates each migration:
  [ADR-0214](0214-gpu-parity-ci-gate.md).
- Touched-file lint contract:
  [ADR-0141](0141-touched-file-cleanup-rule.md).
- Source: `req` — the parent agent's brief explicitly scoped this PR to
  templates-only, citing the sister-agent's analysis that "CUDA + Vulkan
  kernels share no concrete shape" and that per-backend savings are
  modest (paraphrased to neutral English per the project's user-quote
  rule).
