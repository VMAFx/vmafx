<!-- markdownlint-disable MD050 MD060 -->
# Research-0753: CUDA Resolution-Aware Dispatch — Design Rationale

**Date**: 2026-05-29
**Author**: lusoris
**Status**: Published
**Related ADR**: [ADR-0753](../adr/0753-cuda-resolution-aware-dispatch.md)

## Summary

Multi-resolution profiling (Research-0748, Research-0749, Research-0751) showed
that CUDA kernel occupancy optimisations are resolution-regime-specific. A single
dispatch classifier (`WS_SMALL` / `WS_MEDIUM` / `WS_LARGE`) keyed on luma pixel
count enables each feature extractor to pick the right variant at runtime.

## Motivation

### Measured data driving the design

| Metric / Opt              | 576p (WS_SMALL)   | 1080p (WS_MEDIUM)    | 4K (WS_LARGE)     |
| ------------------------- | ----------------- | -------------------- | ----------------- |
| adm_cm `__launch_bounds__ | Neutral (< 1 wave)| −9.3% kernel time    | −0.3% (noise)     |
| filter1d `__ldg`           | Neutral (0.76w)   | +3.6% end-to-end VIF | Saturated (253w)  |
| ms_ssim_decimate smem      | 95% L1 hit rate   | 95% L1 hit rate      | 95% L1 hit rate   |
| motion CUDA vs CPU         | CPU wins          | CUDA wins            | CUDA wins         |

At 576p, every ADM kernel runs < 1 wave. Occupancy hints that save registers
but increase instruction count are net-zero at this wave count. At 4K the same
kernel is register-pressure-free (enough waves to absorb the full register
budget without stalling).

### Why a pixel-count threshold rather than a wave-count query

Wave count is a function of resolution, block size, and SM count — all of which
are known at dispatch time. Resolution is the most stable predictor (it does not
change across runs on the same clip) and the cheapest to compute (one multiply).
A per-SM-count branch would add a `cuDeviceGetAttribute` call in `init_fex_cuda`
and a second multiply at dispatch time, with marginal improvement over the
resolution proxy.

### Threshold choice

- `VMAF_CUDA_PIXEL_THRESHOLD_MEDIUM = 1280 * 720 = 921600` aligns with the
  industry-standard HD boundary. The measured 576p operating point (186624 px)
  falls well within WS_SMALL; a letterboxed 720p60 (921600 px) is right at the
  boundary and classified WS_MEDIUM, which is conservative (it will get the
  bounded variant — safe, because the bounded variant is never worse than neutral
  at 720p; the occupancy win materialises fully at 1080p).
- `VMAF_CUDA_PIXEL_THRESHOLD_LARGE = 3840 * 2160 = 8294400` aligns with the
  4K UHD boundary. The transition from medium to large at this point matches
  the profiling data showing zero gain for `adm_cm __launch_bounds__` at 4K.

## Scaffolding pattern

The dispatch infrastructure is intentionally minimal:

1. `resolution_dispatch.h` — header-only enum + one function decl.
2. `resolution_dispatch.c` — ~20 lines of pure C; no CUDA headers; unit-testable
   in the CPU-only build.
3. Each extractor adds: one extra `CUfunction` pointer, one `cuModuleGetFunction`
   call in init, one `if (ws == WS_MEDIUM)` branch at the launch site.

This is deliberately not a generic "dispatch table" framework. A flat branch in
each extractor is more readable, easier to audit, and avoids the indirection cost
of a table lookup on a hot path (although the hot path is the GPU kernel, not this
branch, so the distinction is academic).

## Future expansion

The policy table in ADR-0753 has empty rows for:

- **motion CPU fallback at WS_SMALL**: at 576p CUDA loses to CPU outright. A
  future PR could call the CPU motion path when `ws == WS_SMALL`, bypassing the
  CUDA kernel entirely. This requires refactoring the extractor to hold a CPU
  fallback path alongside the GPU path, which is a larger change.
- **filter1d no-bounds at WS_LARGE**: at 4K the kernel is wave-saturated;
  `__ldg` routes through the read-only L1 cache and remains beneficial, but the
  `__launch_bounds__` part of ADR-0743 is neutral. A future PR could split the
  filter1d variants along the same WS_MEDIUM / WS_LARGE axis.
- **ssim_vert_combine memory layout at WS_LARGE**: the DRAM-bound behaviour at
  55.8% (Research-0738) worsens at 4K. A tiled smem approach could help but
  requires a new numerical-contract review.

## Correctness

The two `adm_cm` kernel variants are mathematically identical — they differ only
in the `__launch_bounds__` annotation that constrains register allocation.
The cross-backend parity gate (`places=4`) is unchanged because the annotation
does not affect the computation, only register spilling decisions.
