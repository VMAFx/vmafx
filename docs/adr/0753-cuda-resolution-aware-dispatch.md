<!-- markdownlint-disable MD013 MD060 -->
# ADR-0753: Runtime Resolution-Aware CUDA Kernel Variant Dispatch

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `cuda`, `perf`, `build`

## Context

Multi-resolution profiling (Research-0748, Research-0749, Research-0751) demonstrated
that no single CUDA kernel configuration is optimal across all workload sizes. The
fork targets three distinct operating points:

- **576p (WS_SMALL)**: nearly every CUDA metric kernel is launch-overhead-bound. The
  ADM family runs < 1 wave at this resolution; `adm_cm` `__launch_bounds__(128,8)`
  shows zero gain. Motion loses to CPU outright. Occupancy optimisations are neutral
  because there are not enough waves to hide latency.
- **1080p (WS_MEDIUM)**: wave count is large enough for occupancy effects to
  materialise. `adm_cm_line_kernel_8` with `__launch_bounds__(128,8)` saves −9.3%
  execution time by reducing register-bank pressure; `filter1d_8_horizontal_kernel`
  `__ldg() + __launch_bounds__(128,10)` gives +3.6% end-to-end VIF throughput.
  ms_ssim_decimate is already saturated (95% L1 hit rate) — no tiling needed.
- **4K (WS_LARGE)**: filter1d is fully saturated (253 waves, 69.7% active warps);
  `__ldg()` / `__launch_bounds__` for that kernel are neutral. `adm_cm`
  `__launch_bounds__` shows near-zero effect (−0.3%, noise). Shared-memory tiling
  for the ADM / ms_ssim families is now beneficial because working-sets exceed L2.

Choosing the variant at compile time would require separate binaries or fat-binary
tricks. A cheap runtime branch (one integer comparison per frame at kernel-launch
time) is the right trade-off.

## Decision

We will classify every incoming frame pair into one of three workload classes
(`WS_SMALL`, `WS_MEDIUM`, `WS_LARGE`) based on pixel count at the luma plane,
using the thresholds below. Per-kernel optimisation policies are then expressed
as a lookup in a flat dispatch table keyed on `(feature, workload_class)`. The
classification lives in a new header+TU (`resolution_dispatch.h` /
`resolution_dispatch.c`) under `core/src/feature/cuda/`.

Resolution thresholds (luma pixel count):

| Class      | Condition                               | Canonical example |
| ---------- | --------------------------------------- | ----------------- |
| `WS_SMALL` | `w * h < 1280 * 720`                    | 576p (576×324)    |
| `WS_MEDIUM`| `1280*720 <= w*h < 3840*2160`           | 1080p (1920×1080) |
| `WS_LARGE` | `w * h >= 3840 * 2160`                  | 4K (3840×2160)    |

Optimisation application policy (initial, based on measured data):

| Optimisation                                  | WS_SMALL | WS_MEDIUM | WS_LARGE |
| --------------------------------------------- | -------- | --------- | -------- |
| `adm_cm` `__launch_bounds__(128,8)`           | SKIP     | APPLY     | SKIP     |
| `filter1d` `__ldg + __launch_bounds__(128,10)`| SKIP     | APPLY     | APPLY    |
| `ssim_vert_combine` `__ldg + __launch_bounds` | SKIP     | APPLY     | APPLY (confirm via 4K re-measurement follow-up if needed) |
| `ms_ssim_decimate` smem tiling               | SKIP     | SKIP      | SKIP     |
| motion CUDA vs CPU fallback                   | CPU      | CUDA      | CUDA     |

Three feature extractors now use this dispatch framework:

1. **`adm_cm_device()` in `integer_adm_cuda.c`**: selects between
   `adm_cm_line_kernel_8` (bounded, WS_MEDIUM) and `adm_cm_line_kernel_8_no_bounds`
   (WS_SMALL / WS_LARGE). `AdmStateCuda` carries `func_adm_cm_line_kernel_8_no_bounds`.

2. **`filter1d_8()` in `integer_vif_cuda.c`**: selects between
   `filter1d_8_horizontal_kernel_2_17_9` (bounded, WS_MEDIUM + WS_LARGE) and
   `filter1d_8_horizontal_kernel_2_17_9_no_bounds` (WS_SMALL). `VifStateCuda`
   carries `func_filter1d_8_horizontal_kernel_2_17_9_no_bounds`.

3. **`submit_fex_cuda()` in `integer_ssim_cuda.c`**: selects between
   `calculate_ssim_vert_combine` (bounded, WS_MEDIUM + WS_LARGE) and
   `calculate_ssim_vert_combine_no_bounds` (WS_SMALL). `SsimStateCuda`
   carries `func_vert_no_bounds`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Compile-time variants only (separate build flags) | Zero runtime overhead; simpler code | Requires separate binaries for each resolution class; inconvenient for users | Defeats single-binary deployment story |
| First-frame auto-tuning (launch both variants, keep faster) | Self-calibrates to actual hardware | Adds 1-2 frame latency; requires result buffering; complex timing infrastructure | Implementation complexity outweighs marginal calibration benefit; profiling data already provides good starting thresholds |
| Hand-tuned single variant (always-bounds or never-bounds) | Simplest code | −9.3% regression at 1080p (no-bounds) or neutral waste at 576p/4K (always-bounds) | Not neutral — leaves measured gain on the table |
| Per-SM-count auto-selection (query `cuDeviceGetAttribute`) | Hardware-aware rather than resolution-proxy | A second GPU property query per init; resolution is the primary occupancy driver, SM count a secondary one | More complexity for marginal improvement; resolution is the dominant parameter |

## Consequences

- **Positive**: `adm_cm` at 1080p recovers the −9.3% that would otherwise be left
  on the table when the `__launch_bounds__` variant is unconditionally applied at all
  resolutions.
- **Positive**: The dispatch policy is O(1) per-frame — a single `w*h` multiply and
  integer compare at kernel-launch time.
- **Positive**: The table is the single source of truth for "which optimisation wins
  where"; future kernel authors add one row and one `cuModuleGetFunction` call.
- **Negative**: `AdmStateCuda` grows one additional `CUfunction` pointer per
  resolution-split kernel. Currently one (`func_adm_cm_line_kernel_8_no_bounds`).
- **Negative**: `adm_cm.cu` grows a second kernel instantiation, increasing cubin
  size marginally (a few KB per arch target).
- **Neutral**: The `vmaf_cuda_workload_class` function is pure C and needs no CUDA
  headers, making it testable with the CPU-only build.
- **Neutral / follow-ups**: Future expansions (motion CPU fallback at 576p,
  `filter1d` at WS_LARGE) follow the same pattern without touching the dispatch
  infrastructure.

## References

- Research-0748: `filter1d_8_horizontal_kernel` 1080p re-measurement.
- Research-0749: ADM CM `__launch_bounds__` profiling at 1080p.
- Research-0751: Cross-backend 4K baseline + PR #79 `adm_cm` A/B at 4K.
- [ADR-0743](0743-cuda-vif-filter1d-ncu-driven-perf.md) — VIF filter1d register
  pressure ceiling pattern.
- [ADR-0750](0750-adm-cm-cuda-launch-bounds.md) — `adm_cm_line_kernel_8`
  `__launch_bounds__(128,8)` measurement and policy.
- req: "design (not implement) a dispatch policy that picks the right variant at
  runtime based on detected workload size. Scaffold the C-side glue + ADR
  documenting the policy."
