# Research digest 0091 — cambi × {CUDA, SYCL, HIP} GPU port planning

- **Date**: 2026-05-09
- **Author**: Lusoris + Claude (Anthropic)
- **Companion ADR**: [ADR-0345](../adr/0345-cambi-gpu-port-strategy.md)
- **Parent ADRs**: [ADR-0205](../adr/0205-cambi-gpu-feasibility.md)
  (feasibility verdict — Strategy II hybrid),
  [ADR-0210](../adr/0210-cambi-vulkan-integration.md)
  (Vulkan integration that landed as PR #196 / T7-36)
- **Predecessor digests**: [Research-0020](0020-cambi-gpu-strategies.md)
  (strategy comparison), [Research-0032](0032-cambi-vulkan-integration.md)
  (Vulkan integration-time trade-offs), [Research-0090](0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
  §Ordering rationale (which deferred this round)
- **Status**: feeds the decision in ADR-0345

## Question

Research-0090 (PR #520) closed the GPU-coverage long-tail audit with the
plan "the remaining 8 kernels port-by-PR" and ordered the work as
chroma-PSNR → chroma-SSIM/MS-SSIM → cambi. The cambi row was tagged
"5× more complex than the chroma extensions" and explicitly deferred
to this round. PR #520 §Ordering rationale: "`cambi` × {CUDA, SYCL}
last. ... **Defer to a separate planning round** once chroma is in —
the cambi backlog is large enough to warrant its own ADR(s)."

This digest is that planning round. Three operational questions:

1. **Which Strategy** — full-GPU (Strategy III, see Research-0020) or
   host-staged (Strategy II, the Vulkan precedent in ADR-0210) — for
   each of the three pending backends (CUDA, SYCL, HIP)?
2. **What does each port cost** in LOC and what are the per-backend
   risks?
3. **In what order** should the three backend ports land?

The deliverables are this digest plus ADR-0345. No implementation code.
Per-backend port PRs follow per the ordered plan.

## TL;DR

- **Strategy II for all three backends** (CUDA, SYCL, HIP). Same
  reasoning as ADR-0205 / ADR-0210: the precision-sensitive
  `calculate_c_values` sliding-histogram pass remains on the host;
  the embarrassingly-parallel pre-passes (preprocess, derivative,
  spatial-mask SAT, decimate, filter-mode) ship as device kernels;
  the host residual runs unmodified CPU code on byte-identical
  buffers, preserving `places=4` by construction.
- **LOC estimates** (anchored to ADR-0210's Vulkan v1 size and the
  in-tree CUDA / SYCL port shapes for comparable multi-stage features):
  CUDA ~1100 LOC, SYCL ~1300 LOC, HIP ~1100 LOC.
- **Risk rating**: CUDA = LOW (most-tested toolchain, nearest-neighbour
  to Vulkan kernel shape), SYCL = MEDIUM (oneAPI + AdaptiveCpp dual
  toolchain per ADR-0335 doubles the test surface), HIP = MEDIUM-LOW
  (post-PR #499 the HIP runtime is no longer -ENOSYS, but only six
  consumer features are wired so far — cambi would be the most
  complex HIP consumer to date).
- **Order**: CUDA first, SYCL second, HIP third. Rationale per §6.

## Background — what already exists

| Path | Where | LOC | Notes |
| --- | --- | ---: | --- |
| Scalar CPU + dispatcher | `core/src/feature/cambi.c` | 1619 | Hot path is `calculate_c_values` (sliding 1024-bin histogram per output column, range +1 / -1 updates per row). |
| AVX2 SIMD twin | `core/src/feature/x86/cambi_avx2.c` + `.h` | 95 + hdr | Vectorises the histogram increment / decrement within a single row. Row-to-row state stays sequential. |
| AVX-512 SIMD twin | `core/src/feature/x86/cambi_avx512.c` + `.h` | 94 + hdr | Same shape as AVX2, wider lanes. PR #493 (T3-9). |
| NEON SIMD twin | `core/src/feature/arm64/cambi_neon.c` + `.h` | 95 + hdr | ARM equivalent. PR #493 (T3-9). |
| Internal helper header | `core/src/feature/cambi_internal.h` | 113 | Exposes file-static helpers (`vmaf_cambi_calculate_c_values`, `vmaf_cambi_decimate`, `vmaf_cambi_filter_mode`, `vmaf_cambi_get_spatial_mask`, `vmaf_cambi_preprocessing`, `vmaf_cambi_default_callbacks`) for GPU twins per ADR-0210 §Decision point 3. |
| Vulkan host glue | `core/src/feature/vulkan/cambi_vulkan.c` | 1368 | Shipped by PR #196 (T7-36). Mirrors the scaffold from ADR-0210. |
| Vulkan shaders | `core/src/feature/vulkan/shaders/cambi_*.comp` | 548 total | 5 files: preprocess (99), decimate (64), derivative (103), filter_mode (112), mask_dp (170 — fuses row-SAT, col-SAT, threshold via `PASS` spec const). |

CUDA, SYCL, HIP have **no** cambi twin today. ADR-0192 §Out of scope
parked them for "follow per-backend cadence after the Vulkan terminus
lands"; ADR-0210 §Out of scope reaffirmed the deferral.

## §1. Strategy choice per backend

The three GPU re-formulations from Research-0020:

| Strategy | Summary | Bit-exactness | Dev cost | GPU utilisation |
| --- | --- | --- | --- | --- |
| **I — single-WG direct port** | One workgroup walks the histogram sequentially. | Yes | Low | Catastrophic (1/64 of GPU active). |
| **II — host-staged hybrid** | GPU does pre-passes; host does sliding histogram + pooling. | Yes (by construction). | Modest. | Partial — pre-passes accelerate; histogram phase doesn't. |
| **III — fully-on-GPU per-pixel histogram** | Each thread re-scans its 65×65 window (~4225 reads). | Yes (per-pixel histogram is order-insensitive after the in-pixel reduction). | Higher. | Full grid; cache-bound. |

ADR-0205 §Decision picked Strategy II for Vulkan; ADR-0210
§Alternatives confirmed it survives empirical scrutiny. The same
analysis applies verbatim to CUDA / SYCL / HIP because:

1. The structural blocker (1024-bin histogram per output column,
   sequential row-major update) is **algorithmic**, not hardware-
   specific. It pessimises the same way on NVIDIA SMs, Intel Xe-cores,
   AMD CUs, and Lavapipe's CPU SIMD lanes.
2. The host residual cost is dominated by a single `calculate_c_values`
   invocation per scale on host CPU. No GPU acceleration of that phase
   shows up in any of the three backends regardless of vendor.
3. The PCIe / HSA shuttle cost is bounded by the same per-scale
   `(image, mask)` buffer pair as Vulkan (two `uint16` planes per
   scale, ≤50 MiB at 4K scale 0). Not a backend-differentiator.
4. The `places=4` by-construction argument from ADR-0205 §Precision
   contract holds identically: every GPU pre-pass is integer-only,
   readback is byte-identical, host residual runs unmodified CPU
   code. ULP=0 expected on all three backends, same as Vulkan.

**Decision: Strategy II for CUDA, SYCL, and HIP.** Strategy III
remains parked for a future profile-driven PR (see ADR-0205
§Out of scope; tracked separately as the v2 follow-up).

## §2. Multi-scale dispatch shape

Vulkan picked **one shader module dispatched 4 times** per scale
(one decimate + one filter-mode H + one filter-mode V per scale)
rather than 4 specialised modules. The reason was Vulkan-specific:
spec-constant pivots in `cambi_mask_dp.comp` collapse three SAT
passes into one TU, and reusing the same module across scales saves
on `VkShaderModule` overhead.

This argument **does not transfer cleanly** to CUDA / SYCL / HIP:

- **CUDA** kernels are JIT-compiled per `<<<grid, block>>>` invocation
  (or AOT via `nvcc`); there is no per-launch module overhead worth
  optimising. The natural shape is one `__global__` per kernel role
  (decimate, derivative, filter-mode, mask-SAT) launched 4× per
  scale. This matches the existing CUDA feature ports (`integer_adm/
  adm_dwt2.cu` etc. — one kernel per role).
- **SYCL** uses `parallel_for` lambdas; `sycl::queue::submit` overhead
  is the binding constraint, not module count. A single TU
  (`integer_cambi_sycl.cpp`) holds all kernels as nested lambdas, the
  same shape `integer_adm_sycl.cpp` (1663 LOC) and `integer_ms_ssim_
  sycl.cpp` already use.
- **HIP** mirrors CUDA structurally. One `__global__` per role.

**Decision: one kernel per role per backend, dispatched 4 times for
the multi-scale loop.** Vulkan's `PASS` spec-const pattern is
preserved as a Vulkan-only optimisation.

## §3. Histogram-update reuse — defer to v2

Research-0090 §Ordering rationale flagged the
`77474251` / `8c60dc9e` upstream optimisations PR #463 landed on the
CPU sliding-histogram path. Those patches:

- Skip-band: skip histogram updates when the contrast difference falls
  outside `[c_low, c_high]` (the visibility window).
- LUT-based reciprocal computation: replaces a runtime divide with a
  table lookup.

In Strategy II these optimisations live on the **host residual**
(they're inside `calculate_c_values`, which stays on CPU). They are
therefore inherited automatically by every Strategy II backend with
zero additional GPU work. **No backend-port PR needs to touch the
GPU side for skip-band / LUT.**

If Strategy III ever lands (v2), those optimisations *do* matter for
the GPU kernel and would need separate ADRs per backend. Out of scope
for this planning round.

## §4. Memory layout

ADR-0210 §Decision point 5 picked **per-dispatch one-shot command
buffer** (not a persistent pool) for Vulkan, mirroring the
`psnr_vulkan.c` precedent. The corresponding decisions per pending
backend:

| Backend | State allocation | Per-CTU intermediate | Justification |
| --- | --- | --- | --- |
| CUDA | `VmafCudaPicture` for each per-scale `(image, mask)` pair, allocated once at `init()` and reused across frames. Mirrors `integer_adm_cuda.c::init()`. | Global memory; SAT row-prefix-sum stays in global with shared-memory tile reductions. The SAT is small (≤4K × 4K × `int32` = 64 MiB worst case); an in-shared-mem tile pivot is premature optimisation. | Matches the in-tree CUDA pattern; no novel allocation surface. |
| SYCL | `sycl::malloc_device` per-scale buffers held in `CambiStateSycl`, freed in `close()`. Mirrors `integer_ms_ssim_sycl.cpp`. | Global memory same as CUDA. SAT computed via `sycl::group_reduce` for tile-internal scans, decoupled lookback for cross-tile. | Matches the in-tree SYCL pattern. |
| HIP | `hipMalloc` per-scale buffers, lifetime same as CUDA. | Global memory same as CUDA; SAT via `__shared__` tile reductions. | Mirrors `adm_hip.c` / `vif_hip.c`. |

**No new memory primitive is required for any of the three backends.**
Each port reuses the existing per-backend allocation pattern.

## §5. Estimated LOC + risk per backend

Anchored against:

- **Vulkan reference** (PR #196 / ADR-0210): 1368 LOC host glue +
  548 LOC shaders = **1916 LOC total** for cambi.
- **CUDA reference** (`integer_adm_cuda.c`): 1364 LOC host glue +
  ~1330 LOC across 5 `.cu` files = **~2700 LOC** for ADM (the
  closest in-tree CUDA feature in pipeline shape — multi-scale,
  multi-stage, integer-arithmetic state machine).
- **SYCL reference** (`integer_adm_sycl.cpp`): 1663 LOC single TU
  with nested lambdas.
- **HIP reference** (`adm_hip.c`): scaffold-only at the moment;
  comparison drawn against the active feature wiring scope per
  ADR-0241 (`integer_psnr_hip.c` ≈ 600 LOC end-to-end including
  all kernels).

The cambi GPU port is materially simpler than ADM in two ways:

1. **No DWT** — no wavelet pyramid, no per-scale CSF weights table.
   The cambi GPU side is plain integer mask + 7×7 box-sum, half
   the kernel count.
2. **No accumulator reduction** — the pooled score reduction stays
   on the host residual. There's no equivalent of ADM's CSF-CM
   final accumulator that ADM has to reduce on-device.

The cambi port is materially harder than chroma-PSNR in three ways:

1. **Multi-scale loop** — 5 dispatches per kernel role, 4 kernel
   roles, vs PSNR's single-dispatch shape.
2. **SAT phase** — separable 2D prefix-sum with cross-WG synchronisation,
   vs PSNR's per-pixel SSE accumulator.
3. **Host residual integration** — must invoke `vmaf_cambi_calculate_
   c_values` against GPU-produced buffers, vs PSNR's pure on-device
   pipeline.

| Backend | Host glue | Kernels | Smoke + gate | Total LOC | Risk |
| --- | ---: | ---: | ---: | ---: | --- |
| **CUDA** | ~700 | ~350 (5 `.cu` files: `cambi_preprocess.cu`, `cambi_derivative.cu`, `cambi_decimate.cu`, `cambi_filter_mode.cu`, `cambi_mask_dp.cu`) | ~50 | **~1100** | **LOW** — closest to Vulkan kernel shape, in-tree CUDA infrastructure mature, kernel translation from `cambi_*.comp` is mechanical (GLSL→CUDA-C). |
| **SYCL** | ~1200 | (in-TU lambdas, ~250 LOC additional inside the host glue) | ~50 | **~1300** | **MEDIUM** — single-TU shape inflates host glue size; AdaptiveCpp + oneAPI dual-toolchain (ADR-0335) doubles the test matrix. SAT phase needs `sycl::group_reduce` + decoupled-lookback shimming. |
| **HIP** | ~700 | ~350 (5 `.hip` files, hipify-perl seeded from CUDA) | ~50 | **~1100** | **MEDIUM-LOW** — structural mirror of CUDA via hipify-perl is well-trodden; the medium adjustment reflects that cambi would be the most complex HIP consumer to date (post-PR #499 only six consumer features are wired). |

LOC numbers are estimates; per memory `feedback_no_guessing` they are
anchored to the cited in-tree references — Vulkan `cambi` (1916), CUDA
`integer_adm` (2700), SYCL `integer_adm` (1663), HIP `integer_psnr`
(~600). The cambi GPU side has fewer kernel roles than ADM (no DWT,
no per-scale CSF weights), so the per-backend total comes in below
the ADM anchor. The Vulkan figure is the closest direct comparable
(same algorithm, same Strategy II), so CUDA and HIP track it within
±15 %. SYCL inflates because the single-TU style merges shader code
into host code; the LOC delta is structural rather than work-volume.

## §6. Implementation ordering

Three considerations:

1. **Maintainer-test surface**: which backend has the most-mature
   local + CI test harness today?
2. **Code-reuse upstream**: which port produces a template the next
   port can lean on?
3. **Risk concentration**: which port should land first to surface
   blocker bugs early?

| Backend | Maintainer-test maturity | Template upstream value | Risk | Suggested order |
| --- | --- | --- | --- | --- |
| CUDA | High — most-tested fork backend; CI lane on GPU runner; local validation against Lawrence's GPU box. | High — the kernel translation pattern (`.comp` → `.cu`) is reusable for HIP via `hipify-perl`. | Low. | **1st** |
| SYCL | High — oneAPI + AdaptiveCpp lanes (ADR-0335) shipped; Intel Arc + AMD via AdaptiveCpp tested. | Medium — single-TU shape is structurally distinct; HIP cannot lift from it. | Medium. | **2nd** |
| HIP | Medium — runtime landed PR #499; only six consumers wired; CI lane installs ROCm via PR #500. | Low — terminal port; nothing downstream depends on its template. | Medium-low. | **3rd** |

**Recommended order: CUDA → SYCL → HIP.**

The CUDA-first ordering preserves the same CPU → CUDA → SYCL → HIP
cadence ADR-0192 set for the GPU long-tail batch 3 metrics; the
existing 8-kernel chroma-extension queue per Research-0090 has been
landing in the same order (PR #520 CUDA, PR #527 SYCL, HIP pending).
Continuing the cadence avoids context switches on the maintainer
side and lets the CUDA `.cu` files seed the HIP `.hip` files via
hipify-perl after the SYCL port lands.

## §7. Per-port PR shape (for the implementing PRs that follow)

Each backend port PR carries:

1. The host glue file (`cambi_<backend>.c` or `.cpp`).
2. The kernel files (CUDA: 5× `.cu`; SYCL: in-TU lambdas; HIP: 5×
   `.hip`). The kernels mirror the Vulkan shaders 1:1.
3. Build wiring: meson `cambi_<backend>` source list +
   `feature_extractor.c::feature_extractor_list[]` registration.
4. Cross-backend gate row in `scripts/ci/cross_backend_vif_diff.py`'s
   `FEATURE_METRICS` for `cambi`. Per ADR-0205 §Precision contract
   the gate runs at `places=4` from day one — no two-step ratchet.
5. The six ADR-0108 deliverables. Per-port PR ships its own narrow
   ADR (e.g. `ADR-0NNN-cambi-cuda-port.md`) that cites this digest
   and ADR-0345 as the design parents; the per-port ADR's `##
   Alternatives considered` is "no alternatives — Strategy II
   inherited from ADR-0345" if no new architectural decision is
   made at port time.
6. `cambi_internal.h` is unmodified by the GPU twin. The header
   already exposes `vmaf_cambi_calculate_c_values` /
   `vmaf_cambi_spatial_pooling` / `vmaf_cambi_decimate` etc. for
   the host residual.

## §8. Out of scope (deferred — not blockers for this round)

- **Strategy III v2** (fully-on-GPU `calculate_c_values`). Tracked
  per ADR-0205 §Out of scope; needs profile data on per-pixel 65×65
  scan cache-hit-rate before a focused ADR can land.
- **High-res-speedup option's GPU path**. ADR-0210 §Out of scope
  applies to all three pending backends — Strategy II honours the
  option at the per-scale level but doesn't add the ~2× perf
  shortcut the CPU has at 1080p+. v2.
- **GPU heatmap dump**. Strategy II keeps `heatmaps_path` host-only
  on all backends.
- **HIP via hipify-perl mechanical port**. The HIP port is planned
  as a separate PR seeded from the CUDA `.cu` files via
  `hipify-perl` per the standard fork pattern; no new ADR-level
  decision needed beyond the slot reservation here.

## §9. Open question for the implementing PRs

When the SYCL port lands, it will be the first cambi consumer to run
through both oneAPI (icpx) and AdaptiveCpp (ADR-0335). The Vulkan
port's bit-exact-by-construction argument relies on integer-only GPU
phases. **AdaptiveCpp's `sycl::group_reduce` lowering on AMD**
(stdpar / generic SSCP) is the one place where bit-exactness on cambi
needs empirical confirmation, because the reduction primitives in
AdaptiveCpp differ from those in oneAPI. The SYCL port PR is the
right place to surface this — not this planning round. Flagged here
so the SYCL implementor knows to add a smoke-fixture row for both
toolchains, not just oneAPI.

## §10. Six deep-dive deliverables — meta-commentary

This digest is the **(1) research digest** deliverable for the
planning round (ADR-0345 is the companion ADR). The other five
deliverables (decision matrix, AGENTS.md invariant, reproducer,
CHANGELOG, rebase-notes) attach to ADR-0345's PR per the standard
ADR-0108 template. Each per-backend port PR that follows ships its
own six-deliverables set as usual.

## References

- Parent ADR: [ADR-0345](../adr/0345-cambi-gpu-port-strategy.md) —
  decision: Strategy II for CUDA, SYCL, HIP.
- Predecessor ADR: [ADR-0205](../adr/0205-cambi-gpu-feasibility.md) —
  feasibility verdict, Strategy II hybrid for the cambi GPU port.
- Predecessor ADR: [ADR-0210](../adr/0210-cambi-vulkan-integration.md) —
  Vulkan v1 implementation that landed as PR #196 / T7-36.
- Predecessor digest:
  [Research-0020](0020-cambi-gpu-strategies.md) — full strategy
  comparison + bandwidth analysis + literature survey.
- Predecessor digest:
  [Research-0032](0032-cambi-vulkan-integration.md) — Vulkan
  integration-time trade-offs.
- Predecessor digest:
  [Research-0090](0090-t3-15-gpu-coverage-long-tail-2026-05-09.md)
  §Ordering rationale — deferred this round.
- Sibling ADR: [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batch 3 scope.
- Sibling ADR: [ADR-0212](../adr/0212-hip-backend-scaffold.md) — HIP
  backend scaffold.
- Sibling ADR: [ADR-0241](../adr/0241-hip-first-consumer-psnr.md) —
  HIP first consumer cadence.
- Sibling ADR: [ADR-0335](../adr/0335-sycl-adaptivecpp-second-toolchain.md)
  (in-flight in PR #498) — dual SYCL toolchain scope.
- CPU reference: [`core/src/feature/cambi.c`](../../core/src/feature/cambi.c)
  (1619 LOC) + SIMD twins + `cambi_internal.h`.
- Vulkan reference: [`core/src/feature/vulkan/cambi_vulkan.c`](../../core/src/feature/vulkan/cambi_vulkan.c)
  (1368 LOC) + 5 shaders (548 LOC).
- CUDA template reference: `core/src/feature/cuda/integer_adm_cuda.c`
  (1364 LOC) + `integer_adm/*.cu` (~1330 LOC).
- SYCL template reference: `core/src/feature/sycl/integer_adm_sycl.cpp`
  (1663 LOC).
- HIP template reference: `core/src/feature/hip/integer_psnr_hip.c`
  `adm_hip.c`.
- User direction: standing CLAUDE.md §12 r10/r11 (every fork-local PR
  ships the six deep-dive deliverables; doc-substance rule applies);
  standing CLAUDE.md §13 (prefer closing the matrix over chasing peak
  utilisation); memory `feedback_no_guessing` (LOC + risk estimates
  cite in-tree comparables).
