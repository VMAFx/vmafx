# ADR-0210: cambi Vulkan integration (Strategy II hybrid)

- **Status**: Accepted
- **Date**: 2026-04-29
- **Deciders**: <lusoris@pm.me>, Claude (Anthropic)
- **Tags**: vulkan, gpu, cambi, feature-extractor, fork-local, places-4

## Context

[ADR-0205](0205-cambi-gpu-feasibility.md) closed the cambi GPU
feasibility spike with the verdict "ship hybrid host/GPU" and
deferred the actual integration to a follow-up PR (backlog item
**T7-36**, see also the §A.1.3 Section-A audit decisions). This ADR
locks the v1 implementation that lands in that follow-up.

After the spike PR, the in-tree state was:

- `core/src/feature/vulkan/cambi_vulkan.c` — 200-line scaffold
  with `init_stub` / `extract_stub` / `close_stub` returning
  `-ENOSYS`. Not wired into `feature_extractor_list[]`.
- `core/src/feature/vulkan/shaders/{cambi_decimate,cambi_derivative,cambi_filter_mode}.comp`
  — 3 of the 6 shaders ADR-0205 specifies. Compiled but unused.

This PR implements:

1. **The 2 missing shaders** (`cambi_preprocess.comp` +
   `cambi_mask_dp.comp` — the latter unifies row-SAT, col-SAT, and
   threshold-compare passes via a `PASS` spec constant).
2. **The full Vulkan-aware lifecycle** in `cambi_vulkan.c`
   (descriptor pool / pipeline build / dispatch sequencer / readback
   path) replacing the `_stub` triple.
3. **The host residual call path** — `vmaf_cambi_calculate_c_values`
   patial pooling — invoked against the GPU-produced image + mask
   buffers via a small `cambi_internal.h` header that exposes cambi.c's
   file-static helpers without disturbing CPU SIMD callsites.
4. **Build wiring** — registers the 5 shaders in
   `vulkan_shader_sources[]`, the TU in `vulkan_sources`, and
   `vmaf_fex_cambi_vulkan` in `feature_extractor.c`'s
   `feature_extractor_list[]`.
5. **Cross-backend gate** — adds a `cambi` row to
   `scripts/ci/cross_backend_vif_diff.py`'s `FEATURE_METRICS` so the
   places=4 contract is checked end-to-end against the CPU.

## Decision

### v1 architecture (recap from ADR-0205)

```text
                           cambi_vulkan_extract()
                                   │
   ┌───────────────────────────────┼───────────────────────────────┐
   │                               │                               │
   ▼                               ▼                               ▼
Host preprocess              GPU spatial mask                 GPU per-scale loop
(CPU vmaf_cambi_             (one pass / frame):              (5 scales):
  preprocessing)               cambi_derivative.comp           cambi_decimate (2×)
  → pics[0] luma              cambi_mask_dp PASS=0,1,2         cambi_filter_mode H+V
                                → mask_buf                     → image_buf, mask_buf
                                                              ─── readback ───

                              Host residual (CPU):
                                vmaf_cambi_calculate_c_values
                                vmaf_cambi_spatial_pooling
                                vmaf_cambi_weight_scores_per_scale
                                MIN(score, cambi_max_val)
```

### Key implementation choices

1. **Strategy II scope**. Per ADR-0205 §Decision the
   precision-sensitive `calculate_c_values` sliding-histogram pass
   and the top-K spatial pooling stay on the host. The GPU services
   every other phase — preprocess (forward-compatible scaffold; v1
   uses the CPU bilinear-resize for bit-exactness on resolution
   mismatches), per-pixel derivative kernel, the 7×7 spatial mask
   SAT + threshold compare, the 2× decimate, and the separable 3-tap
   mode filter.

2. **Mask DP shader fuses 3 passes via spec constant**. Rather than
   write `cambi_mask_sat_row.comp` + `cambi_mask_sat_col.comp` +
   `cambi_mask_threshold.comp` as three separate TUs, the single
   `cambi_mask_dp.comp` uses `PASS = 0/1/2` to switch between
   row-SAT (single thread per row sequential prefix-sum), col-SAT
   (single thread per column), and per-pixel 4-corner box-sum +
   threshold compare. This keeps the build's shader count low and
   collocates the related kernels — easier to review than 3 near-
   identical files.

3. **`cambi_internal.h` instead of buffer-pair refactor**. ADR-0205
   §Decision suggested refactoring `calculate_c_values` to take a
   buffer pair instead of a `VmafPicture *`. v1 takes a different
   tactic: keep `calculate_c_values`'s `VmafPicture *` signature,
   expose it (and `decimate` / `filter_mode` / `get_spatial_mask` /
   `cambi_preprocessing` / `spatial_pooling` / `weight_scores_per_scale` /
   `get_pixels_in_window`) through a thin internal header, and have
   the Vulkan twin allocate its own `VmafPicture` pair as readback
   targets. This avoids rippling through every CPU SIMD callsite for
   `increment_range` / `decrement_range` / `get_derivative_data_for_row`,
   which all expect `(uint16_t *, ...)` not `(VmafPicture *, ...)`.
   The cost is one full-frame readback per scale — negligible vs the
   per-scale GPU dispatch cost.

4. **Bit-exactness preserved by construction**. Every GPU phase is
   integer arithmetic (`uint16` derivative, `int32` SAT, `>` compare,
   stride-2 gather, 3-element `mode3` lookup). The readback into the
   `VmafPicture` pair is byte-identical to what the CPU code path
   would have written in-place. The host residual then runs the
   *exact same* CPU code on those buffers, so the emitted score is
   bit-identical to `vmaf_fex_cambi`. ULP=0 / max_abs_diff=0 against
   the CPU on the smoke fixture; cross-backend gate at `places=4`
   per ADR-0205's tightened contract.

5. **Free-set descriptor pool, per-dispatch one-shot command buffer**.
   Mirrors `psnr_vulkan.c` (T7-23 part 1) — small max_sets pool with
   `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`, allocate +
   write + bind + dispatch + submit + wait + free per stage. v1
   trades latency for code clarity; a v2 perf pass can batch all
   per-scale dispatches into one command buffer if profiling
   warrants.

### Out of scope (deferred)

- Fully-on-GPU `calculate_c_values` (Strategy III) — tracked for v2.
- Heatmap dump on GPU.
- CUDA + SYCL twins — follow per ADR-0192 cadence (one PR per
  backend).
- High-res-speedup option's GPU path. v1 honours the option at the
  per-scale level (decimate first, then build mask) but doesn't add
  the ~2× perf shortcut the CPU has at 1080p+. v2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| **Strategy II hybrid (chosen)** | Bit-exact w.r.t. CPU; closes the matrix at ADR-0192's terminus; modest LOC. | C-values phase doesn't accelerate; PCIe shuttle per scale. | **Selected** — ships the closure now, leaves Strategy III as a focused v2 PR. |
| **Strategy III — fully-on-GPU c-values** | Algorithmically clean; full GPU utilisation. | ~9× CPU bandwidth on a per-output-pixel 4225-read scan; we have no profile data on cache-hit rate at this access pattern. ~800 LOC additional. | Premature without profile data. Tracked as a v2 follow-up ADR. |
| **Buffer-pair refactor of `calculate_c_values`** | Cleaner host-residual call surface; no `VmafPicture` allocation in the GPU twin. | Ripples through every CPU SIMD callsite + the AVX2/AVX-512/NEON paths' `inc_range_callback` signatures; ~200 LOC refactor diff to cambi.c + 3 SIMD TUs to gain a few ns of redundant pointer chasing. | Cost > benefit. The internal-header trampoline lets the GPU twin land without touching CPU SIMD code. |
| **Three separate mask-DP TUs (row-SAT, col-SAT, threshold)** | Each shader trivially understandable in isolation. | 3 near-identical files = 3 spv_embed.h headers + 3 pipeline-build sites; reviewers would have to diff three files to see the SAT semantics. | Single TU + `PASS` spec const = one file to review, one shader module on the device, one `cambi_mask_dp_spv.h` artefact. |

## Consequences

- **Positive**: closes the GPU long-tail matrix terminus declared in
  ADR-0192. Every registered feature extractor in the fork now has
  at least one GPU twin (lpips remains delegated to ORT execution
  providers per ADR-0022).
- **Positive**: establishes a reusable Strategy II template for any
  future metric whose state is too sequential for full SIMT
  parallelism. cambi was the worst case in the matrix; if Strategy II
  worked here it works everywhere.
- **Positive**: keeps the cross-backend gate tight at `places=4` —
  no per-metric tolerance carve-outs.
- **Negative**: cambi GPU performance in v1 is bottlenecked by the
  CPU c-values phase. At 4K this remains ~250 ms / frame. Real users
  who care about cambi throughput will need to wait for v2 (Strategy
  III) or stick with CPU.
- **Negative**: adds maintenance surface that mirrors CPU code. The
  `vmaf_cambi_calculate_c_values` host residual stays in lock-step
  with the CPU extractor; any CPU-side change to the c-value formula
  requires touching both sites. Mitigated by sharing helpers via the
  internal header.

## Precision investigation

The places=4 contract holds **by construction**, not by empirical
adjustment:

- All GPU phases are integer arithmetic (no float rounding).
- The host residual runs the unmodified CPU c-values code on
  byte-identical buffers.
- Therefore the emitted score is bit-identical to the CPU path on
  any input.

The smoke fixture (Netflix `ref_576x324_48f.yuv` ↔ `dis_576x324_48f.yuv`,
48 frames) confirms ULP=0 / max_abs_diff=0 (the per-frame Cambi
score readout matches the CPU path byte-for-byte through the JSON
output). Cross-backend gate runs at the canonical `places=4` from
day one.

## References

- Parent: [ADR-0205](0205-cambi-gpu-feasibility.md) — feasibility
  spike, Strategy II hybrid decision.
- Sibling: [ADR-0201](0201-ssimulacra2-vulkan-kernel.md) — direct
  precedent for the hybrid host/GPU split.
- Companion: [docs/research/0020-cambi-gpu-strategies.md](../research/0020-cambi-gpu-strategies.md)
  — strategy comparison.
- Companion: [docs/research/0032-cambi-vulkan-integration.md](../research/0032-cambi-vulkan-integration.md)
  — integration-time trade-offs (this ADR's research digest).
- CPU reference: [`core/src/feature/cambi.c`](../../core/src/feature/cambi.c).
- Backlog: T7-36 (cambi GPU integration PR).
- User direction: standing CLAUDE.md §12 r10/r11 (every fork-local
  PR ships the six deep-dive deliverables; doc-substance rule
  applies).
