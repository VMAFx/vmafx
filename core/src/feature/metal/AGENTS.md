<!-- markdownlint-disable MD013 MD060 -->
# `core/src/feature/metal/` — Metal feature-kernel directory

Parent: [../AGENTS.md](../AGENTS.md). The Metal backend runtime lives at
[`../../metal/AGENTS.md`](../../metal/AGENTS.md); ADRs governing this
directory are listed in the "Governing ADRs" section at the bottom of
this file.

## Purpose

Contains one `.mm` (Objective-C++ host dispatch) + one `.metal` (Metal
Shading Language device kernel) pair per feature extractor in the Metal
GPU backend, plus the per-T8-1 scaffold `.c` stubs that were superseded.

Only `.mm` + `.metal` pairs are functional. The `.c` stubs (e.g.
`float_psnr_metal.c`) are replaced by their `.mm` counterparts once
a real kernel lands; they are removed from `metal_sources` in
`core/src/metal/meson.build` when the conversion happens.

## Rebase-sensitive invariants

- **Only the wired `.mm` + `.metal` pairs exist** (ADR-0545). The
  Metal feature directory carries exactly one wired `.mm` + `.metal`
  pair per registered extractor, each with a meson entry and a registry
  slot in `feature_extractor.c`. The wired set is the full 17-kernel
  cross-backend metric set: `integer_motion_v2` (registers as
  `motion_v2_metal`), `float_psnr`, `float_moment`, `integer_psnr`,
  `float_motion`, `integer_motion`, `float_ssim`, `float_ms_ssim`,
  `integer_ssim`, `float_vif`, `integer_vif`, `float_adm`,
  `integer_adm`, `integer_ciede`, `integer_psnr_hvs`, `integer_cambi`,
  and `ssimulacra2`.
  (`float_ansnr` was removed from the wired set in commit 70ed8b3ce3 / PR #38.)
  All 17 are live: each has a paired `.metal` kernel, a meson entry, a
  registry slot, and a per-kernel CPU-vs-Metal parity test guarded by
  `core/test/test_metal_kernel_coverage_audit.c`. The only remaining
  Metal-twin gap is the SpEED family (`speed_chroma` / `speed_temporal`),
  which has CUDA/SYCL/HIP twins but no Metal kernel yet. Do **not** add a
  new `<feature>_metal.mm` without (a) the matching `.metal` kernel,
  (b) a meson entry in `metal_objcpp_lib`, a `<feature>_air`
  `custom_target`, and an entry in `metal_air_files`, (c) an
  `extern VmafFeatureExtractor` declaration in `feature_extractor.c`
  plus a slot in `feature_extractor_list[]` under the `HAVE_METAL`
  block, (d) a row in `g_metal_features[]` in
  `core/src/metal/dispatch_strategy.c` for both the registry name and
  every provided-features key, and (e) a basename entry (with bumped
  `EXPECTED_KERNEL_COUNT`) in `test_metal_kernel_coverage_audit.c`.

- **Per-WG float/uint partials — no atomics**: Apple MSL does not
  expose `atomic_ulong` (`atomic_fetch_add_explicit` for `ulong`
  silently compiles but fails on device — confirmed CI run 25685703780
  / job 75408804495). All Metal kernels use a per-threadgroup
  `float`/`uint` partials array indexed by
  `bid.y * grid_groups.x + bid.x`, reduced on the host in `double`.
  Do not introduce `atomic_ulong` or `atomic_fetch_add_explicit`
  for 64-bit types.

- **`simd_sum` reduction**: MSL `simd_sum()` is the standard two-level
  reduction primitive. All kernels use:
  1. `simd_sum(per_thread_val)` → lane 0 of each SIMD group writes to
     a `threadgroup float simd_partials[8]` array.
  2. Thread 0 (`lid == 0`) sums the `simd_count` SIMD-group partials
     into the global `partials[bid.y * grid_groups.x + bid.x]` slot.

- **8×16 threadgroup / 20×20 shared tile (radius-2 kernels)**:
  `integer_motion_v2`, `float_motion`, `integer_motion`,
  and `float_ssim` all use a 16×16 threadgroup with a 20×20 shared
  tile (4-element halo radius-2). (`float_ansnr` used the same
  tile layout but was removed in commit 70ed8b3ce3 / PR #38.) Tile pitch is 21 (not 20) to avoid
  bank conflicts on Apple GPU 32-bank threadgroup memory.

- **Per-WG partials buffer**: each `.mm` allocates a Shared-storage
  `MTLBuffer` sized `ceil(W/16) * ceil(H/16)` float (or uint)
  elements, one per threadgroup. For `float_moment`, the buffer holds
  4 floats per threadgroup (interleaved ref1st/dis1st/ref2nd/dis2nd).

- **Bridge-retained PSO slots**: each `.mm` stores
  `MTLComputePipelineState` handles as `void *` under
  `__bridge_retained` cast (one per bpc variant). `close_fex_metal`
  must release them via `__bridge_transfer` to avoid leaks.

- **`float_moment` feature name correction**: the T8-1 scaffold
  `float_moment_metal.c` erroneously listed `{"float_moment1",
  "float_moment2", "float_std", NULL}` as `provided_features`. The
  correct names (matching CPU, CUDA, HIP, SYCL, Vulkan) are
  `{"float_moment_ref1st", "float_moment_dis1st",
  "float_moment_ref2nd", "float_moment_dis2nd", NULL}`. The `.mm`
  conversion uses the correct names; the `.c` file is removed from
  `metal_sources` on merge.

## Kernel files

| File                               | Status      | Feature(s)                                                              |
|------------------------------------|-------------|-------------------------------------------------------------------------|
| `integer_motion_v2.metal`          | Done (T8-1c) | `VMAF_integer_feature_motion_v2_sad_score`, `motion2_v2_score`         |
| `integer_motion_v2_metal.mm`       | Done (T8-1c) | host dispatch                                                           |
| `float_psnr.metal`                 | Done (T8-1d) | `float_psnr`                                                            |
| `float_psnr_metal.mm`              | Done (T8-1d) | host dispatch                                                           |
| `float_moment.metal`               | Done (T8-1e) | `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`, `float_moment_dis2nd` |
| `float_moment_metal.mm`            | Done (T8-1e) | host dispatch (fixes provided_features)                                 |
| `integer_psnr.metal`               | Done (T8-1g) | `psnr_y`, `psnr_cb`, `psnr_cr`                                          |
| `integer_psnr_metal.mm`            | Done (T8-1g) | host dispatch                                                           |
| `float_motion.metal`               | Done (T8-1h) | `float_motion`                                                          |
| `float_motion_metal.mm`            | Done (T8-1h) | host dispatch                                                           |
| `integer_motion.metal`             | Done (T8-1i) | `VMAF_integer_feature_motion_y_score`, `VMAF_integer_feature_motion2_score` |
| `integer_motion_metal.mm`          | Done (T8-1i) | host dispatch                                                           |
| `float_ssim.metal`                 | Done (T8-1j) | `float_ssim`, `float_ssim_l`, `float_ssim_c`, `float_ssim_s`           |
| `float_ssim_metal.mm`              | Done (T8-1j) | host dispatch                                                           |
| `float_ms_ssim.metal`              | Done (T8-2b) | `float_ms_ssim` — 5-scale pyramid, Wang weights                         |
| `float_ms_ssim_metal.mm`           | Done (T8-2b) | host dispatch (ADR-0490, wired in meson per ADR-0545)                   |
| `integer_ssim.metal`               | Done         | `ssim`                                                                  |
| `integer_ssim_metal.mm`            | Done         | host dispatch                                                           |
| `float_vif.metal`                  | Done         | `VMAF_feature_vif_scale0..3_score`, `vif`, `vif_num/den` (+ per-scale) |
| `float_vif_metal.mm`               | Done         | host dispatch                                                           |
| `integer_vif.metal`                | Done         | `VMAF_integer_feature_vif_scale0..3_score`, `integer_vif` (+ per-scale)|
| `integer_vif_metal.mm`             | Done         | host dispatch                                                           |
| `float_adm.metal`                  | Done         | `VMAF_feature_adm2/aim/adm3/adm_scale0..3_score`, `adm_num/den` (+scale)|
| `float_adm_metal.mm`               | Done         | host dispatch                                                           |
| `integer_adm.metal`                | Done         | `VMAF_integer_feature_adm2/aim/adm3_score`, `integer_adm` (+ per-scale)|
| `integer_adm_metal.mm`             | Done         | host dispatch                                                           |
| `integer_ciede.metal`              | Done         | `ciede2000`                                                             |
| `integer_ciede_metal.mm`           | Done         | host dispatch                                                           |
| `integer_psnr_hvs.metal`           | Done         | `psnr_hvs_y`, `psnr_hvs_cb`, `psnr_hvs_cr`, `psnr_hvs`                  |
| `integer_psnr_hvs_metal.mm`        | Done         | host dispatch                                                           |
| `integer_cambi.metal`              | Done         | `Cambi_feature_cambi_score`                                            |
| `integer_cambi_metal.mm`           | Done         | host dispatch                                                           |
| `ssimulacra2.metal`                | Done         | `ssimulacra2`                                                           |
| `ssimulacra2_metal.mm`             | Done         | host dispatch                                                           |

## Rebase-sensitive invariants (end-of-stream drain + frame-0 motion2)

- **Every Metal extractor MUST set `VMAF_FEATURE_EXTRACTOR_METAL`**
  (`feature_extractor.h`, bit 7). `flush_context_serial`
  (`core/src/libvmaf.c`) drains a backend's pending final-frame
  `collect()` *only* when the extractor carries its backend flag — the
  `#ifdef HAVE_METAL` branch keys off `VMAF_FEATURE_EXTRACTOR_METAL &&
  gpu_pending`, mirroring the CUDA / HIP / SYCL drain blocks. An
  extractor that forgets the flag silently drops its last submitted
  frame's score (the generic submit/collect double-buffer leaves
  `collect(N)` pending). When adding a new `<feature>_metal.mm`, OR the
  flag into `.flags` alongside any feature-class flag (e.g.
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL`).
  All 17 registered Metal extractors set this flag (GAP-METAL-DISPATCH-FLAGS-ZERO-MODEL-FALLBACK
  resolved 2026-09-02; all 9 round-3/4 extractors promoted).
- **Frame-0 `motion2` emission contract.** Motion-family Metal
  extractors append `motion2 = 0.0` at index 0, a no-op at index 1, and
  `min(prev, cur)` at index − 1 for index ≥ 2 — byte-identical to
  `integer_motion_metal` and the HIP / CUDA twins. `float_motion_metal`
  was previously missing the index-0 append and double-wrote at index 1
  (fixed fix/metal-drain-motion2, 2026-06-20). Do not "simplify" the
  index-0 / index-1 split away.

## Rebase-sensitive invariants (motion_fps_weight)

- **`motion_fps_weight` cross-backend parity** — see the canonical
  invariant note in [`../cuda/AGENTS.md`](../cuda/AGENTS.md).
  `integer_motion_v2_metal.mm` and `float_motion_metal.mm` both carry
  the `motion_fps_weight` option and apply it identically to the
  CUDA / SYCL / Vulkan / HIP twins: `motion_v2` applies the weight in
  `flush()` to both scores before the min; `float_motion` applies it
  in `collect()` (index >= 2, to both `w_cur` and `w_prev` before the
  min) and in `flush()` (scaled tail emission). When
  `motion_fps_weight = 1.0` (default) the arithmetic is a no-op and
  the `places=4` gate must pass. Any future change to the weight
  application math must span all motion-family GPU twins in the same PR.

## Per-feature option-table sync invariant

- **GPU twins must mirror the CPU option table for model-configured features.**
  When a model (such as the default model `vmaf_v1.0.16_3d0h`) provides feature options,
  `vmaf_use_features_from_model` checks that the selected backend's feature extractor
  supports every option present in the model's options dictionary. If any option is missing
  from the GPU twin's `options[]` table, dispatch rejects the twin and falls back to CPU.
  For CAMBI (`integer_cambi_metal`), `cambi_high_res_speedup` (alias `hrs`, default 0, min 0, max 2160)
  must be present in the option table and honored during processing (adjusting window size and
  subsampling post-spatial-mask for resolutions >= 1080p, matching `cambi.c`).
- **Do not re-declare shared CAMBI constants.** The resolution thresholds
  (`CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p` / `_1440p` / `_2160p`) and
  `CAMBI_WINDOW_DIVISOR` live in `core/src/feature/cambi_internal.h`, which
  `integer_cambi_metal.mm` already includes. A Metal-local copy of a shared
  constant is how a twin silently drifts from `cambi.c` on the next upstream
  sync.
- **`submit_fex_metal` must leave `s->d_image` / `s->d_mask` / `s->d_tmp` pointing at the
  allocations `init_fex_metal` made.** The per-scale pipeline rotates the three scratch
  buffers via pointer swaps; every exit path restores the originals so the next frame
  starts from a known state and `close_fex_metal` releases the handles it owns.
- **`vmaf_use_feature()` consumes the option dictionary.** It takes ownership of the
  `VmafFeatureDictionary` on every path except the argument-validation guards, so a
  CPU-vs-Metal parity test must build a fresh dictionary per call. Sharing one across
  the two runners is a use-after-free; freeing it afterwards is a double free.

## Governing ADRs

- [ADR-1176](../../../../docs/adr/1176-metal-motion-v2-mirror-closeout.md) — Metal motion_v2 mirror closeout and reflect-101 parity
- [ADR-0490](../../../../docs/adr/0490-float-ms-ssim-metal-port.md) — T8-2b: float_ms_ssim_metal port
- [ADR-0421](../../../../docs/adr/0421-metal-first-kernel-motion-v2.md) — T8-1c through T8-1k batch specification
- [ADR-0420](../../../../docs/adr/0420-metal-backend-runtime-t8-1b.md) — runtime (T8-1b), prerequisite
- [ADR-0361](../../../../docs/adr/0361-metal-compute-backend.md) — scaffold (T8-1), origin
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) — `places=4` cross-backend parity gate

## motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_metal` emits `motion3_v2_score` host-side in its
  flush, mirroring the CPU `integer_motion_v2.c::flush` and the CUDA twin
  byte-for-byte: per-frame `motion_blend(motion2, blend_factor,
  blend_offset)` then `MIN(_, motion_max_val)` clip, a `stamp_value` seed
  for `i < min_idx (= 1)`, and an optional 2-tap `motion_moving_average`,
  via the shared `motion_blend_tools.h` helper. Any change to the CPU
  flush blend/clip/seed/average logic must be mirrored into all four GPU
  twins (cuda/sycl/hip/metal) in the same PR to keep the `places=4`
  `test_metal_motion_v2_parity` gate green.

## mv2_mirror cross-twin invariant (ADR-1176)

- **mv2_mirror is reflect-101, identical across backends**: `integer_motion_v2.metal::mv2_mirror`
  uses iterated reflect-101 `idx = (idx < 0) ? -idx : 2 * (sup - 1) - idx`, bit-identical
  to CPU `integer_motion_v2.c::mirror`, CUDA `motion_v2_score.cu::mv2_mirror`,
  SYCL `integer_motion_v2_sycl.cpp::dev_mirror_mv2`, and HIP `motion_v2_score.hip::mv2_mirror`.
  Do not revert to the single-bounce or `- 1` edge-replicating form.
