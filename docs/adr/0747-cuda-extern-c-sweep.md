# ADR-0747 — CUDA `extern "C"` invariant for host-looked-up kernels

**Status:** Accepted
**Date:** 2026-05-28
**Deciders:** lusoris
**Related:** [Research-0747](../research/research-0747-cuda-extern-c-sweep.md)

## Context

nvcc compiles `.cu` files as C++. Any `__global__` kernel that is not
declared inside an `extern "C"` block receives a C++ mangled symbol in
the compiled PTX/cubin. When the host uses `cuModuleGetFunction` to
resolve kernels by their plain C names, a mangled symbol causes
`CUDA_ERROR_NOT_FOUND`, silently disabling the feature.

PR #77 fixed one instance of this pattern in
`integer_ssim/ssim_score.cu`. A full audit (Research-0747) identified
one additional broken file introduced after PR #77:
`integer_ssim/integer_ssim_score.cu`, which provides the `"ssim"`
feature on the CUDA backend. All three of its entry points
(`integer_ssim_horiz_8bpc`, `integer_ssim_horiz_16bpc`,
`integer_ssim_vert_combine`) were missing `extern "C"` wrapping,
silently breaking `--feature ssim --backend cuda` since the file was
introduced.

## Decision

1. Wrap all three `__global__` entry points in
   `integer_ssim/integer_ssim_score.cu` inside `extern "C" { }`.
2. Add a CI script (`scripts/dev/check-cuda-extern-c.sh`) that fails
   if any `__global__` function referenced by `cuModuleGetFunction` is
   not covered by an `extern "C"` block.
3. Add a mandatory invariant to `core/src/feature/cuda/AGENTS.md` so
   that all future contributors are informed of the requirement before
   writing new CUDA kernel TUs.

## Alternatives considered

- **Rename all kernels to a `C`-friendly symbol scheme using NVRTC
  or embedding a `__attribute__((visibility("default")))` compiler
  annotation**: rejected. These approaches are more complex and do not
  address the root cause (C++ name-mangling). `extern "C"` is the
  standard CUDA idiom used by the NVIDIA SDK examples and by every
  other kernel in this codebase.
- **Switch from `cuModuleGetFunction` (driver API) to the CUDA runtime
  API**: out of scope for this PR. The driver API is intentional per
  ADR-0001 (deferred CUDA context creation). Changing the dispatch
  pattern would require pervasive refactoring across all 15 host
  extractor files.
- **No-op (accept the breakage)**: rejected. `--feature ssim --backend
  cuda` silently produces no output in the broken state, which violates
  the correctness-first principle (see CLAUDE.md §6 and
  `feedback_correctness_first.md` memory entry).

## Consequences

- `--feature ssim --backend cuda` is restored to working order.
- The CI script prevents future regressions of this class.
- No numerical changes: `extern "C"` affects only symbol naming, not
  kernel code generation.
- The fix is a one-file, three-line insertion; rebase risk is minimal.

## References

- Research-0747 (`docs/research/research-0747-cuda-extern-c-sweep.md`)
- `core/src/feature/cuda/ssim_cuda.c:122–127` (host lookup sites)
- `core/src/meson.build:784` (PTX generation confirming which `.cu`
  maps to `integer_ssim_score_ptx`)
