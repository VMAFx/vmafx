---
paths:
  - core/meson.build
  - core/tools/meson.build
invariant: Orientation for core C engine workflows, governing ADRs, backend subtree orientation, and build targets.
area: build-and-toolchains
---
<!-- markdownlint-disable MD013 MD060 -->
# Workflows routed here

| Task | Skill |
| --- | --- |
| Add feature extractor | [../.claude/skills/add-feature-extractor/SKILL.md](../../.claude/skills/add-feature-extractor/SKILL.md) |
| Add SIMD path (AVX2 / AVX-512 / NEON) | [../.claude/skills/add-simd-path/SKILL.md](../../.claude/skills/add-simd-path/SKILL.md) |
| Add GPU backend (CUDA / SYCL / HIP / Vulkan) | [../.claude/skills/add-gpu-backend/SKILL.md](../../.claude/skills/add-gpu-backend/SKILL.md) |
| Register model JSON | [../.claude/skills/add-model/SKILL.md](../../.claude/skills/add-model/SKILL.md) |
| Cross-backend numeric diff | [../.claude/skills/cross-backend-diff/SKILL.md](../../.claude/skills/cross-backend-diff/SKILL.md) |
| Profile hot path | [../.claude/skills/profile-hotpath/SKILL.md](../../.claude/skills/profile-hotpath/SKILL.md) |

## Governing ADRs

- [ADR-0119](../../docs/adr/0119-cli-precision-default-revert.md) — CLI precision default `%.6f` (Netflix-compat golden gate); `--precision=max` opts in to `%.17g`. Propagates to `output.c` and Python. Supersedes [ADR-0006](../../docs/adr/0006-cli-precision-17g-default.md).
- [ADR-0012](../../docs/adr/0012-coding-standards-jpl-cert-misra.md) — coding-standards stack.
- [ADR-0022](../../docs/adr/0022-inference-runtime-onnx.md) — execution-provider mapping ORT↔backends.
- [ADR-0024](../../docs/adr/0024-netflix-golden-preserved.md) — golden-data gate (three CPU reference pairs, never modified).
- [ADR-0025](../../docs/adr/0025-copyright-handling-dual-notice.md) — dual-copyright policy.
- [ADR-0137](../../docs/adr/0137-thread-local-locale-for-numeric-io.md) —
  thread-local locale abstraction (`thread_locale.h`) for all numeric I/O.
- [ADR-1182](../../docs/adr/1182-windows-utf8-path-contract.md) —
  Windows UTF-8 path contract and internal path shims.
- [ADR-1320](../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md) —
  CUDA fatbin and HIP HSACO kernel header dependency tracking via explicit depend_files and compiler depfiles.
- [ADR-1333](../../docs/adr/1333-meson-test-secret-env-sanitization.md) —
  Meson parent-environment runner plus default test-setup sanitization.

Backend-specific orientation:

- [src/cuda/AGENTS.md](../src/cuda/AGENTS.md) — CUDA backend runtime
- [src/sycl/AGENTS.md](../src/sycl/AGENTS.md) — SYCL backend runtime
- [src/vulkan/AGENTS.md](../src/vulkan/AGENTS.md) — Vulkan backend runtime
- [src/dnn/AGENTS.md](../src/dnn/AGENTS.md) — ONNX Runtime integration (tiny AI)
- [src/feature/AGENTS.md](../src/feature/AGENTS.md) — feature extractors + SIMD
- [test/AGENTS.md](../test/AGENTS.md) — C unit tests

## Build

```bash
meson setup build [-Denable_cuda=true|false] [-Denable_sycl=true|false] [-Denable_dnn=auto]
ninja -C build
python3 ../scripts/ci/run_meson_test.py -- -C build
```

Shortcut: `/build-vmaf --backend=cpu|cuda|sycl|all`.
