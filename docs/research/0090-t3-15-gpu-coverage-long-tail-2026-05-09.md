<!-- markdownlint-disable MD013 MD060 -->
# Research 0090 — T3-15 GPU coverage long-tail batch 4: actual gap re-audit + per-kernel ordering

- **Date**: 2026-05-09
- **Author**: Lusoris, Claude (Opus 4.7)
- **Tracking**: backlog T3-15 (replaces T3-17 + T3-18)
- **Companion ADR(s)**: ADR-0192 (long-tail terminus, Status: Accepted, frozen per ADR-0028);
  follow-up ADR for chroma-CUDA TBD when the first port lands.
- **Tags**: `gpu`, `cuda`, `sycl`, `vulkan`, `feature-extractor`, `chroma`, `cambi`, `motion3`,
  `audit`

## Why this digest exists

The backlog row for T3-15 in `.workingdir2/BACKLOG.md` (line 328) carries a 2026-04-28
description that is materially stale as of 2026-05-09. Two of its three sub-rows are
**already mostly closed**, and a third ((c) `motion3`) was closed in its default mode
by ADR-0219. Before opening any implementation PR we need an audit that re-states the
real gap, otherwise the next agent will re-port kernels that already exist.

This digest is the source-of-truth gap re-audit; the BACKLOG row will be updated in the
implementation PR(s) that close each sub-row.

## What the BACKLOG row claims vs. what the tree actually has

| Sub-row claim (BACKLOG.md line 328)                              | Tree reality (verified 2026-05-09)                                                                                                                                                                                                                              |
|------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| (a) "CUDA/SYCL ship integer adm/vif/motion only — 3 of ~19 metrics" | False. `core/src/feature/cuda/` ships 16 kernels, `core/src/feature/sycl/` ships 14. Per [`metrics-backends-matrix.md` lines 66-91](../../.workingdir2/analysis/metrics-backends-matrix.md), every classical metric except `cambi` already has CUDA + SYCL twins. The actual remaining (a)-class gap is **`cambi` × {CUDA, SYCL}** = 2 kernels. |
| (b) "PSNR Vulkan v1 + CUDA both luma-only because picture_vulkan / picture_cuda upload paths can't carry chroma" | Half-stale. Vulkan PSNR went chroma-complete in PR #204 / [ADR-0216](../adr/0216-vulkan-chroma-psnr.md) (chroma claim re: `picture_vulkan` was wrong on inspection — it's a generic byte allocator). CUDA `picture_cuda.c` already loops over all 3 planes (lines 44-48). Real gap: the per-extractor `provided_features` lists in `integer_psnr_cuda.c:216` and `integer_psnr_sycl.cpp:244` still claim `{"psnr_y", NULL}` and dispatch only luma. |
| (c) "motion3 (5-frame window) GPU coverage on Vulkan + CUDA + SYCL" | Half-stale. `motion3_score` in 3-frame mode (the default, the only mode any shipped model uses) is emitted by all three GPU backends — see `integer_motion_cuda.c:354-360`, `integer_motion_sycl.cpp:496-502`, `motion_vulkan.c:23-29` and [ADR-0219](../adr/0219-motion3-gpu-coverage.md). What's deferred is `motion_five_frame_window=true` mode, which all three backends correctly reject with `-ENOTSUP`. ADR-0219's own §Decision explicitly accepts that deferral (no shipped model uses 5-frame; the option exists for ad-hoc CLI tuning only). |

## Real gap inventory (corrected)

The actual open kernel count is **8**, not 16. Ordered for ascending implementation
risk (mirrors ADR-0192's "ascending complexity" pattern):

| # | Kernel                              | Backend | Closest existing twin (reference)                            | Est. LOC | Risk | Unblocks                              |
|---|-------------------------------------|---------|--------------------------------------------------------------|---------:|:----:|----------------------------------------|
| 1 | `psnr_cuda` chroma extension        | CUDA    | `psnr_vulkan.c` (ADR-0216) — same algo, port host scaffolding | ~120     | Low  | chroma-PSNR matrix cell + (3) + (5) ↓  |
| 2 | `psnr_sycl` chroma extension        | SYCL    | `psnr_vulkan.c` (ADR-0216) + `integer_psnr_sycl.cpp` v1       | ~120     | Low  | chroma-PSNR matrix cell + (4) + (6) ↓  |
| 3 | `ssim_cuda` chroma extension        | CUDA    | `float_ssim_cuda.c` (luma) + chroma loop pattern from (1)     | ~150     | Low  | chroma-SSIM matrix cell                |
| 4 | `ssim_sycl` chroma extension        | SYCL    | `float_ssim_sycl.cpp` (luma) + chroma loop pattern from (2)   | ~150     | Low  | chroma-SSIM matrix cell                |
| 5 | `ms_ssim_cuda` chroma extension     | CUDA    | `float_ms_ssim_cuda.c` (luma) + (3)                           | ~180     | Med  | chroma-MS-SSIM matrix cell             |
| 6 | `ms_ssim_sycl` chroma extension     | SYCL    | `float_ms_ssim_sycl.cpp` (luma) + (4)                         | ~180     | Med  | chroma-MS-SSIM matrix cell             |
| 7 | `cambi_cuda`                        | CUDA    | `cambi_vulkan.c` Strategy II hybrid host/GPU per ADR-0210     | ~900     | High | matrix terminus on cambi               |
| 8 | `cambi_sycl`                        | SYCL    | `cambi_vulkan.c` Strategy II + (7)                            | ~900     | High | matrix terminus on cambi               |

Optional / deferred (no implementation in this batch):

- `motion_five_frame_window=true` on Vulkan + CUDA + SYCL — needs 5-deep device-side
  blur ring buffer in 3 kernel languages. ADR-0219 §Context: "no shipped VMAF model
  uses this mode; the option exists for ad-hoc CLI tuning". Re-open only if a model
  surfaces that consumes it.

### Ordering rationale

1. **PSNR chroma first (CUDA → SYCL).** Smallest diff (extend the `provided_features`
   array, allocate per-plane SSE accumulators, dispatch the existing kernel three
   times against per-plane buffers). Mirrors the Vulkan landed pattern in ADR-0216
   exactly. Establishes the chroma-extension idiom every later (3)-(6) kernel
   reuses. The CPU integer_psnr.c reference at lines 1-end already emits
   `psnr_y / psnr_cb / psnr_cr` unconditionally (clamping to luma-only on YUV400);
   parity is the existing CPU contract, not a new specification.
2. **SSIM chroma second.** Reuses the chroma-iteration pattern from (1) on a more
   complex per-plane reduction. Vulkan ssim_vulkan already has chroma support
   (ADR-0188/0189 pattern); CUDA + SYCL twins inherit the host scaffolding.
3. **MS-SSIM chroma third.** Same pattern at one further scale; the multi-scale loop
   is per-plane outside the existing per-scale loop.
4. **`cambi` × {CUDA, SYCL} last.** Highest risk by 5×. ADR-0210 chose Strategy II
   (hybrid host/GPU split) for Vulkan because the precision-sensitive sliding
   histogram in `calculate_c_values` resists straightforward GPU porting; CUDA + SYCL
   ports must mirror that split or pick a different one. ADR-0205 (cambi GPU
   feasibility) is the prerequisite reading. **Defer to a separate planning round**
   once chroma is in — the cambi backlog is large enough to warrant its own
   ADR(s).

### Per-kernel risk notes

- **(1)–(2) PSNR chroma**: bit-exactness vs CPU is the gate. CPU `integer_psnr.c`
  uses int64 SSE accumulator → `mse = sse / (w*h)` → `10 * log10(peak² / mse)`. The
  Vulkan twin (ADR-0216) holds places=4 vs CPU. CUDA + SYCL kernels should hold the
  same: SSE is integer, `log10` is host-side double, no FMA / fast-math regression
  vectors on the device kernel.
- **(3)–(6) SSIM/MS-SSIM chroma**: existing per-scale FMA-contract notes for
  `_vulkan`/`_cuda`/`_sycl` apply to each chroma plane unchanged. Place=4 gate
  per ADR-0214.
- **(7)–(8) cambi**: the host-side sliding histogram in Strategy II is what holds
  precision in the Vulkan port; CUDA + SYCL ports inherit the same ULP envelope
  by design. Risk is integration plumbing, not numerics.

## Why pick gap #1 (CUDA PSNR chroma) as the proof-of-concept first port

- **Smallest diff**: ~120 LOC across `integer_psnr_cuda.c` + a handful of
  `provided_features` / SSE-array refactor lines. No new shader / kernel — the
  `calculate_psnr_kernel_8bpc` / `_16bpc` entry points are plane-agnostic
  identical to Vulkan's `psnr.comp`.
- **Direct precedent**: ADR-0216 already wrote the design for the Vulkan twin.
  The CUDA port translates the per-plane state arrays and the per-plane dispatch
  loop into CUDA's async submit/collect model.
- **Validates the chroma-extension pattern** that gaps #2-#6 all reuse. If CUDA
  PSNR chroma holds places=4 vs CPU on Netflix golden inputs, the path is clear
  for SYCL PSNR + chroma SSIM + chroma MS-SSIM as follow-up PRs.
- **Doesn't compete with cambi**: gap #7-#8 (cambi CUDA + SYCL) need their own
  Strategy II ADR; this digest deliberately defers that planning.

## Cross-backend gate plan for the first port

`scripts/ci/cross_backend_vif_diff.py` (the canonical places=4 gate per
[ADR-0214](../adr/0214-cross-backend-gate-stability.md)) supports
`--feature psnr` `--backend cuda`. Post-port:

```bash
python3 scripts/ci/cross_backend_vif_diff.py \
    --feature psnr --backend cuda --places 4 \
    --ref python/test/resource/yuv/src01_hrc00_576x324.yuv \
    --dis python/test/resource/yuv/src01_hrc01_576x324.yuv \
    --width 576 --height 324 --bpc 8 --pix-fmt yuv420p
```

The gate checks `psnr_y`, `psnr_cb`, `psnr_cr` independently against the CPU
`integer_psnr` reference at places=4. If any plane fails, the kernel doesn't ship.
Per memory `feedback_no_test_weakening`, places=4 is non-negotiable.

## Netflix golden gate impact

Per CLAUDE.md §8, the three CPU golden tests (`src01_hrc00↔hrc01`,
checkerboard 1px, checkerboard 10px) measure CPU `integer_psnr` outputs and never
see the CUDA backend. The `psnr_cuda` extractor extension changes only what
`provided_features` claims and how many dispatches per frame run — CPU output is
untouched, so the golden gate is structurally unaffected. Verify post-build by
running `make test-netflix-golden` against the rebuilt tree.

## Out of scope for this digest's first port

- All seven follow-up kernels (#2-#8). Each is a separate PR after #1 lands and
  proves the pattern.
- The `motion_five_frame_window=true` 5-frame mode. ADR-0219 explicitly defers it.
- HIP backend coverage of any chroma metric (T7-10 audit-first, ADR-0212 — on a
  separate planning track).
- Updating `metrics-backends-matrix.md` with chroma-PSNR cells. The matrix
  already implies chroma support via the row labels; if a future maintainer wants
  per-plane tracking, that's a docs-only ADR.

## References

- BACKLOG.md row T3-15 (line 328) — the stale row this digest re-audits.
- `.workingdir2/analysis/metrics-backends-matrix.md` — coverage source of truth
  (correct as of 2026-04-29; the BACKLOG row is what's stale, not the matrix).
- [ADR-0192](../adr/0192-gpu-long-tail-batch-3.md) — long-tail terminus; frozen
  per ADR-0028. A `### Status update 2026-05-09: T3-15 first port landed
  (CUDA PSNR chroma)` may append to References when the first port merges, per
  CLAUDE.md §12 r8 freeze rule.
- [ADR-0210](../adr/0210-cambi-vulkan-integration.md) — Strategy II hybrid for
  cambi (prerequisite reading for gaps #7-#8).
- [ADR-0214](../adr/0214-cross-backend-gate-stability.md) — places=4 gate.
- [ADR-0216](../adr/0216-vulkan-chroma-psnr.md) — Vulkan PSNR chroma; this
  digest's first-port direct precedent.
- [ADR-0219](../adr/0219-motion3-gpu-coverage.md) — motion3 GPU coverage in
  3-frame mode + 5-frame deferral.
- `core/src/feature/cuda/integer_psnr_cuda.c` lines 40-237 — the file the
  first port edits.
- `core/src/feature/vulkan/psnr_vulkan.c` lines 1-475 — the chroma-extension
  template the CUDA port mirrors.
- `core/src/feature/integer_psnr.c` — CPU reference; defines the
  `psnr_y / psnr_cb / psnr_cr` contract.
- Per-user-direction (popup-equivalent task description, 2026-05-09):
  "Phase A: research digest + one proof-of-concept kernel". Implementation of
  the first port follows this digest's gap #1 ordering.
