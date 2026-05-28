# ADR-0715: Drop Vulkan compute backend

- **Status**: Accepted
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: backend, vulkan, phase-4b, cleanup

## Context

Research-0733 (hardware backend audit, 2026-05-28) evaluated all five GPU backends
against the VMAFX Phase 4b cloud-native deployment model (ADR-0709). The audit
produced a per-backend KEEP/DROP/DEFER table:

- CUDA: KEEP — NVIDIA node pool; first-class in k8s GPU scheduling.
- SYCL: KEEP — Intel primary lane; Arc iGPU in workloads where CUDA is busy.
- HIP: KEEP — AMD ROCm node pool; scaffold now has real HIP kernels.
- Metal: KEEP — Apple Silicon; macOS CI lane.
- Vulkan: **DROP** — no independent k8s node-pool representation; every Vulkan-capable
  device is already covered by CUDA, SYCL, or HIP. Removing Vulkan eliminates
  ~30 000 LOC of GLSL shaders + runtime without losing GPU coverage on any vendor.

In addition to the LOC burden, the Vulkan backend carried three long-standing open
bugs with no clear fix path:

- **T-VK-1.4-BUMP**: Vulkan 1.4 API-version bump blocked by NVIDIA driver 595.71
  FP-contraction regression (ADR-0264). Root cause identified but the `precise`
  decoration fix (ADR-0269) only partially resolved it.
- **T-VK-CIEDE-F32-F64**: ciede2000 f32/f64 precision gap on NVIDIA hardware —
  structural, not fixable without `shaderFloat64` (rejected in ADR-0273).
- **T-VK-VIF-1.4-RESIDUAL-ARC**: VIF scale-2 non-determinism on Intel Arc A380
  at API 1.4 — Phase-3b stronger-fence experiment merged (PR #512) but residual
  remained open.

## Decision

Remove the Vulkan backend entirely from VMAFX. This includes:

1. `core/src/vulkan/` runtime (picture pool, dispatch, volk, VMA).
2. `core/src/feature/vulkan/` — all 24 feature kernel TUs and GLSL shaders.
3. `core/include/libvmaf/libvmaf_vulkan.h` public header.
4. `core/test/test_vulkan_*.c` and `core/test/test_cambi_vulkan.c`.
5. `core/meson_options.txt` `enable_vulkan` option.
6. All `HAVE_VULKAN` / `#ifdef HAVE_VULKAN` guards throughout the build.
7. `ffmpeg-patches/0004-libvmaf-wire-vulkan-backend-selector.patch` and
   `ffmpeg-patches/0006-libvmaf-add-libvmaf-vulkan-filter.patch`.
8. `docs/backends/vulkan/` and `docs/api/vulkan-image-import.md`.
9. Vulkan CI jobs and workflow matrix entries.
10. Vulkan changelog fragments (44 files removed).
11. `.claude/agents/vulkan-reviewer.md`.

The `--backend` CLI flag no longer accepts `vulkan`; the dispatch fallback order
becomes CUDA > SYCL > HIP > Metal > CPU.

The three open Vulkan bugs are closed by removal.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Vulkan, defer cleanup | No churn in this PR | 3 unresolved bugs persist; ~30 kLOC maintenance burden; no k8s node-pool benefit | Three open bugs with no fix path + zero cloud-native value |
| Keep Vulkan as advisory (CI-optional) | Keeps option open for future lavapipe testing | Still requires maintaining shaders + build system + CI option | ADR-0709 establishes Docker-image-only distribution; Vulkan adds no distinct node pool |
| Move Vulkan to a separate git submodule | Reduces main-tree LOC | Complex submodule lifecycle; CI still needs it wired | Overengineered — the backend is genuinely not needed |

## Consequences

- **Positive**: ~30 000 LOC removed; three open bugs closed; meson setup is faster;
  CI matrix shrinks (no Vulkan SDK install step); ffmpeg-patches series reduces from
  6 to 4 patches.
- **Negative**: Vulkan-specific performance profiles (lavapipe, RADV subgroup path)
  are no longer testable without a full backend reimplementation.
- **Neutral / follow-ups**: Future Vulkan reimplementation (if needed) would start
  from the ADR-0175 scaffold design; the ADR tree is preserved as historical record.
  The `VMAF_FEATURE_EXTRACTOR_VULKAN` enum bit (bit 5) is explicitly reserved in
  `feature_extractor.h` to prevent accidental reuse.

## References

- req: The user directed removal of the Vulkan backend as part of the Phase 4b
  cloud-native redesign, citing Research-0733's KEEP/DROP/DEFER audit result.
- [ADR-0709](0709-vmafx-phase4b-distributed-platform.md) — Phase 4b umbrella
- [ADR-0264](0264-vulkan-1-4-bump-blocked-on-fp-contraction.md) — T-VK-1.4-BUMP root cause
- [ADR-0273](0273-ciede-vulkan-nvidia-f32-f64-precision-gap.md) — T-VK-CIEDE-F32-F64
- [docs/research/0733-hardware-backend-audit-2026-05-28.md](../research/0733-hardware-backend-audit-2026-05-28.md) — Research-0733
