<!-- markdownlint-disable MD060 -->
# AGENTS.md — core/src/feature/sycl

Orientation for agents on per-feature SYCL kernels (DPC++).
Parent: [../AGENTS.md](../AGENTS.md). Backend runtime (queue, USM,
dmabuf import) lives one level up in
[`../../sycl/AGENTS.md`](../../sycl/AGENTS.md).

## Scope

```text
feature/sycl/
  <feature>_sycl.cpp           # one TU per kernel: registration + submit/collect + sycl::queue::submit lambda
```

All TUs compiled with `icpx` (Intel oneAPI) — build line
under [`../../meson.build`](../../meson.build) adds
`-fsycl -fp-model=precise` for every per-kernel TU.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md) +
  [`../../sycl/AGENTS.md`](../../sycl/AGENTS.md)).
- **Every TU is strict-clean regardless of origin or age.** A file ported from
  Netflix, copied from another backend, or present before the ratchet has no
  warning exemption. Its oneAPI compile, SYCL clang-tidy projection, cppcheck,
  and HISS audit must report no file-local diagnostic when touched. Split
  oversized helpers at cohesive phase boundaries; do not add `NOLINT`, lower a
  baseline, or filter a diagnostic to make a lane green. Regenerate the SYCL
  database with `scripts/ci/gen-sycl-compile-commands.py` before clang-tidy.
- **`-fp-model=precise` on SYCL feature line load-bearing.**
  Removing it allows `icpx` to FMA-contract inside kernel
  lambdas, drifting `float_adm_sycl` past `places=4` at scale 2
  (ADR-0202), `ssimulacra2_sycl` past `places=2` through IIR
  (ADR-0206). **It does not stop all contraction** (measured, icpx
  2026.1, ADR-1358): `a * b + c` written as one expression still
  becomes one FMA in 28% of cases, and fp32 `/` and `sqrt` are not
  correctly rounded (28% / 8% differ from the host). A product held
  in a named temporary is not contracted. Kernels that must match
  the host bit for bit need `-ffp-contract=off` after
  `-fp-model=precise` per TU and correctly rounded division / square
  root in the source; `-foffload-fp32-prec-div/-sqrt` act only on the
  final image link. Tracked for the other TUs as
  `T-SYCL-FP-MODEL-PRECISE-CONTRACTS-2026-09-29`.
- **SpEED pipeline arithmetic contract ([ADR-1358](../../../../docs/adr/1358-sycl-speed-device-resident-linalg.md)).**
  Every SpEED kernel lives in `speed_sycl_pipeline.cpp`; the two
  extractor TUs and `speed_sycl_host.cpp` hold none and never wait on
  the queue outside `pipeline_collect()` / `pipeline_wait()`. The four
  TUs are built with `sycl_speed_strict_fp_args` (`-ffp-contract=off`)
  in `core/src/meson.build`. In the pipeline, every division and square
  root goes through `div_rn()` / `sqrt_rn()`, every `log2f` through
  `speed_log2()`, every product feeding an add sits in a named
  temporary, and the fp64 comparisons of `speed.c` go through
  `below_eps()` / `below_eps_scaled()`. The file must not mention the
  fp64 type at all (`core/test/test_sycl_kernel_source_contract.py`).
  On rebase: a plain `/` or `sycl::sqrt` added to a pipeline kernel, or
  a reduction reordered, breaks the bit-exact parity
  `test_sycl_speed_*_parity` measures; keep the order of every sum
  identical to its `speed.c` / `vif_tools.c` reference.
- **fp64-free kernels non-negotiable** ([ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md)).
  Every SYCL feature-kernel lambda captures, operates on `float`
  / integer types only. **No `double` operand inside `parallel_for`
  body**, no `sycl::reduction<double>`, no `sycl::plus<double>`.
  Hard rule, not soft: single fp64 instruction anywhere in
  TU's SPIR-V module causes Level Zero runtime to reject
  entire module on Intel Arc A-series and other fp64-less devices —
  even when offending kernel never submitted.
  - `double` allowed **outside** kernel lambda — host-side
    post-processing in `extract` / `flush` callbacks, score
    aggregation, log10 normalisation.
  - ADM gain limiting uses int64 Q31 (`gain_limit_to_q31` +
    `launch_decouple_csf<false>` in `integer_adm_sycl.cpp`).
  - VIF gain limiting uses fp32 `sycl::fmin`.
- **Kernel identities and output captures have an explicit boundary**
  ([Research-2090](../../../../docs/research/2090-sycl-silent-revert-residuals-2026-09-24.md)).
  Anonymous kernel lambdas in two translation units can receive identical
  generated names, letting the linker pair one launcher's host capture layout
  with the other's device image; that is how the two SpEED TUs collided. Since
  ADR-1358 every SpEED kernel lives in the one TU `speed_sycl_pipeline.cpp`, and
  the source contract rejects a kernel in `speed_chroma_sycl.cpp`,
  `speed_temporal_sycl.cpp` or `speed_sycl_host.cpp`. Do not split the pipeline
  kernels back across TUs.
  `float_psnr_sycl.cpp` and `integer_psnr_sycl.cpp` capture their output
  pointers through `FpsnrOutput` and `PsnrKernelArgs`; do not flatten those
  structs back into raw lambda captures. `integer_moment_sycl.cpp` is the
  remaining scalar-argument shape and aliases `d_sums` to `e_sums` before the
  submit lambda. Keep the alias and use it for all four atomics. The source
  contract in `core/test/test_sycl_kernel_source_contract.py` plants the fp64,
  SpEED host-residual, kernel-outside-pipeline, mid-frame-wait and raw-capture
  regressions and must stay wired into the fast suite.
- **Wholly-new fork files use dual Netflix + Lusoris/Claude
  copyright header** per [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md).
  Most TUs here fork-original SYCL ports of
  Netflix CUDA kernels.

## Twin-update rules

When a SYCL TU has a live CUDA, HIP, or Metal twin, user-visible behavior and
numeric fixes must be reviewed across those twins in the same PR. Vulkan was
removed in ADR-0726 and is not a live twin. The complete CUDA mapping lives in
[`../cuda/AGENTS.md`](../cuda/AGENTS.md). The cross-backend parity gate at
`places=4`
([`scripts/ci/cross_backend_parity_gate.py`](../../../../scripts/ci/cross_backend_parity_gate.py),
ADR-0214) catches drift only after a full GPU run; it does not replace that
source review.

## Parity invariant — motion3 CPU and SYCL moving-average paths

`integer_motion.c` (CPU) and `integer_motion_sycl.cpp` (SYCL) both implement
motion3 post-process as host-side moving average over blended motion2
scores. Both paths **must stay in numerical parity at places=4** (delta
≤ 1e-4, per ADR-0214). Gate enforced by
`core/test/test_sycl_motion3_parity.c`. Any change to blend formula
(`motion_blend()`), moving-average guard condition, or `motion_max_val`
clipping must mirror across both files. Same for CUDA / Vulkan /
HIP / Metal motion twins listed in Twin-update table above — same PR.

## Rebase-sensitive invariants

- **A failed extractor `init` owns its cleanup (BUG-048 section E).** The
  generic feature-extractor framework does not invoke `close` after `init`
  returns an error. Every SYCL init path that has acquired USM, a feature-name
  dictionary, or a graph registration must therefore call its NULL-safe local
  close callback before propagating the error. This is enforced without a GPU
  by `core/test/test_sycl_init_unwind.cpp`; keep the allocator, dictionary, and
  graph fault cases when rebasing any init/close pair. Historical producer
  `709ce470e` was reverted by `5d070b0b4`; the current restoration boundary is
  documented in
  `docs/research/2101-bug048-sycl-init-unwind-restoration-2026-09-24.md`.

- **`integer_motion_sycl.cpp::motion3_postprocess_*` honours
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state.
  `motion_five_frame_window=true` returns `-ENOTSUP` at `init()` with
  `WARNING` log. See [../../AGENTS.md §"motion3_score GPU contract"](../../AGENTS.md).

- **Motion SAD = one shared kernel, difference first
  (T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29).** `motion_sycl` and
  `motion_v2_sycl` both call `motion_sycl_pipeline::enqueue_sad()`
  (`integer_motion_pipeline_sycl.{h,cpp}`): `sum |blur(prev - cur)|`,
  vertical pass rounded `>> bpc`, horizontal `>> 16`, reflect-101 borders =
  CPU `integer_motion.c` / `integer_motion_v2.c` `motion_score_pipeline_*`
  since the Netflix a4a1492d port (PR #532). Bit-exact; gate
  `test_sycl_motion_tiny_frames` compares with `==` (3x3 .. 1283x723, 8/10/16
  bit). `blur(cur) - blur(prev)` rounds twice per pixel -> 2e-4 off at 17x17;
  never reintroduce it. `prev - cur` order load-bearing (arithmetic shift
  floors negatives). Vertical sum int32 up to 15 bpc, int64 at 16 (host picks
  the `submit_sad<Acc>` instance). Kernel lives only in the pipeline TU
  (Research-2090 name collision); extractor TUs hold none. `motion_sycl` keeps
  the raw luma of the previous frame in `d_raw_y[2]` (device memcpy from the
  shared frame after the kernel), because the shared frame buffers are
  overwritten by the next upload. Measured cost vs the old per-frame blur
  (4K micro-benchmark, kernel + copy): about +11% on B580 and UHD 770.

- **`integer_motion_sycl.cpp::motion_add_uv` GPU contract** (ADR-0989).
  When `motion_add_uv=true`, `submit_fex_sycl` packs the reference U and V
  planes into pinned host staging (`h_stage_u` / `h_stage_v`);
  `motion_pre_graph` copies them H2D on the combined queue into
  `d_ref_u[cur_slot]` / `d_ref_v[cur_slot]`; `enqueue_motion_work` runs the
  shared SAD kernel on `d_ref_*[1 - cur_slot]` - `d_ref_*[cur_slot]`,
  accumulating into `d_sad_u` / `d_sad_v`.
  `collect_fex_sycl` sums Y + U + V contributions, each normalized by
  respective plane area (`chroma_w × chroma_h` for UV in YUV420P). The
  numerical gate is the scalar fixed-point oracle in
  `test_sycl_motion_add_uv_parity.c` (ADR-1326), including its required
  960x540 variant. `float_motion(motion_add_uv=true)` has the same semantic
  option but different coefficients and float reduction order, so it is not
  the kernel's numerical oracle.
  CUDA, Vulkan, HIP, and Metal twins expose option but return
  `-ENOTSUP` with `WARNING` until their kernel ports land. On rebase:
  if upstream Netflix adds `motion_add_uv` to `integer_motion.c`, verify
  per-plane normalization formula stays consistent.
  **Queue-sync invariant (T-SYCL-MOTION-ADD-UV-SUBMIT-WAIT-2026-09-29,
  supersedes the ADR-1034 primary-queue wait)**: no host wait in `submit()`.
  UV H2D rides the in-order combined queue in `pre_fn`, ahead of the kernels
  (graph replay is fenced by `ext_oneapi_submit_barrier()`). Staging is
  safe to refill in the next `submit()`: the graph fires on the last
  extractor's submit, after staging, and this extractor's `collect()` of the
  previous frame (`vmaf_sycl_graph_wait`) drained the copy that read it. Do
  not move the UV copies back to `vmaf_sycl_memcpy_h2d_async` (primary queue)
  — that needs the host wait again.

- **`integer_vif_sycl.cpp` rd_stride uses ceiling division for odd widths** (ADR-1034).
  Both `launch_vif_hori_impl` (scalar/SIMD-32) and `launch_vif_fused_impl` (SIMD-16)
  compute downsampled row stride as `(e_w + 1U) / 2U`, not `e_w / 2U`.
  `rd_ref`/`rd_dis` allocation in `init_fex_sycl` uses `((w+1U)/2U) * ((h+1U)/2U)`
  elements. Must stay in sync. On rebase: if future PR modifies
  downsampling path, ensure all three sites (two kernel variants + allocation) use
  same ceiling formula. For even widths/heights result identical to
  truncating division.
  **Reader side too** (T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29):
  `enqueue_vif_work_impl` passes scale s > 0 the stride `(prev_w + 1U) / 2U`
  it was written with, while `cur_w` stays `prev_w / 2` (CPU floor). Reading
  at `cur_w` skewed scales 1-3 on any odd-width scale (17x17 scale1 0.0962 vs
  CPU 0.0765; 854x480 scale3 9.3e-4 off). Guard: `test_sycl_vif_min_dim`
  (17x17, 853x480 at places=4).

- **`integer_vif_sycl.cpp` minimum frame = 16 px, declared through ADR-1324**
  (T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29, maintainer decision: CPU
  fallback). `VIF_MIN_DIM` = max over scales of `(half_width + 1) << s` for
  `vif_fwidth` / `vif_fwidth_rd` (`static_assert` 16): below it a consumed tap
  sits more than one reflection outside the plane -> device lost.
  `.context_check` returns -ENOTSUP below it, `.context_fallback_name = "vif"`
  -> model dispatch (and CLI twin selection) runs the CPU `vif`; direct
  `vif_sycl` fails `init()` with -EINVAL before touching device state. On
  rebase: filter-table change -> update the `static_assert`, keep both guards.

- **`integer_vif_sycl.cpp` warning-clean phase boundaries are load-bearing.**
  Keep `dev_vert_accumulate`, `dev_hori_item_step`, `vif_init_resources`,
  `vif_configure_device`, and `vif_register_graph` as bounded phases. The
  strict C++ profile requires private declarations in anonymous namespaces,
  while HISS-04 applies its 60-line limit to each namespace block as well as
  each function; do not collapse these blocks or replace them with `NOLINT`.
  The tap loops deliberately have no forced-unroll pragma: oneAPI 2026 emits a
  failed-unroll diagnostic for supported target instances and remains free to
  unroll them when profitable. Preserve the `float` device gain in
  `VifHoriLaunchParams`, the arithmetic order inside each phase, and the
  cleanup points in the three init helpers. Base-vs-refactor proof covers
  default and fused modes on 8-bit and 10-bit inputs at zero full-precision
  delta; rerun both modes after an upstream conflict.

- **`integer_psnr_sycl.cpp` honours `enable_chroma` option parity**
  (ADR-0453). `enable_chroma` option (default `true`) clamps `n_planes`
  to 1 in `init_fex_sycl` when set to `false`, matching CPU
  `integer_psnr.c::init`'s behaviour. On rebase: keep clamp and
  `default_val.b = true` aligned with CUDA and Vulkan twins; all three
  backends must agree on default and dispatch logic.

- **`integer_psnr_sycl.cpp` = full CPU `psnr` option table, bit-exact**
  ([ADR-1365](../../../../docs/adr/1365-sycl-twin-cpu-option-parity.md)).
  Device reduces per-plane SSE only. `enable_mse`, `enable_apsnr`,
  `reduced_hbd_peak`, `min_sse`, `uncapped` act on host through
  `feature/psnr_score.h` (`vmaf_psnr_peak` / `_max` / `_from_mse` /
  `_aggregate`) — same helpers as CPU `integer_psnr.c`. `emit_plane()`
  order = CPU (`psnr_*`, then `mse_*`); `collect()` folds SSE + sample
  count into `apsnr_*` totals, `flush_fex_sycl()` publishes aggregates
  after final collect. **On rebase**: no local copy of PSNR math; keep
  option table = CPU table (names, defaults, range, no `FEATURE_PARAM`).
  Guard: `test_sycl_twin_option_parity`.

- **`integer_ssim_sycl.cpp` SSIM twins take CPU options; identical window
  = exactly 1** (ADR-1365). `integer_ssim_sycl`: `enable_db`, `clip_db`;
  `float_ssim_sycl`: `enable_lcs`, `enable_db`, `clip_db`, `scale`. dB
  conversion + ceiling on host (`vmaf_ssim_max_db`,
  `vmaf_ssim_emit_*_named` in `nonfinite_score.h`). Per-window formula:
  every product in a named temporary, variances summed as a pair,
  `numerator == denominator ? 1 : n / d`. Mirrored operation sequence ->
  identical window gives exactly 1 -> `enable_db` = CPU's `+inf` /
  `clip_db` ceiling (ADR-1221), not finite dB of an fp32 residue.
  `enable_lcs` = separate kernel `launch_vert_combine_lcs` (default path
  keeps one reduction), `float_ssim_lcs()` = `iqa/ssim_tools.c` L/C/S in
  fp32 (clamped variances, flat-window covariance clamp, C3 = C2 / 2),
  partials `[l | c | s]`. **On rebase**: do not fold products back into
  expressions (icpx contracts `a * b + c`, ADR-1358) or restore the
  left-to-right four-term variance sum; the identical-frame cases in
  `test_sycl_twin_option_parity` fail on either.

- **`float_motion_sycl.cpp` emits through `motion_clip()`** (ADR-1365).
  Every emitted `motion` / `motion2` (debug score, tail in `flush()`
  included) = `MIN(score * motion_fps_weight, motion_max_val)`, CPU
  `float_motion.c` order (min of two SADs first). `motion_force_zero`
  short-circuits `submit()` (no upload, no kernel) and `collect()` emits
  zeros; before ADR-1365 it was declared and ignored. `motion3` not
  provided (CPU extractor only).

- **`integer_psnr_sycl.cpp` uses ceiling division for chroma plane geometry**
  (PR #878 Vulkan twin fix). `cw` and `ch` computed via
  `(w + 1U) >> 1` / `(h + 1U) >> 1`, not `w / 2U` / `h / 2U`, to match
  CPU + CUDA + Vulkan behaviour on odd-dimension YUV420. On rebase: if
  upstream Netflix changes chroma-dimension formula in
  `integer_psnr.c::init`, propagate here and to CUDA and Vulkan twins
  in same PR.

- **`integer_psnr_hvs_sycl.cpp` DCT lives in local memory, never in one
  work-item's private arrays** (T-SYCL-PSNR-HVS-B580-SIGSEGV-2026-09-29,
  Research-2123). Old shape: work-item 0 ran the whole 8x8 DCT + masking on
  five private 64-element arrays; IGC 2.41.5 SIGSEGVs the host compiling that
  at SIMD32 for Xe2 (Arc B580). Now: `hvs_fdct8_pass()` = one 1-D transform per
  work-item per pass (items 8-23 pass 1 while items 0-1 take the variance
  ratios; items 0-15 pass 2), work-item 0 streams coefficients from local
  memory, `hvs_mask_at()` recomputes the mask per coefficient. Float reductions
  keep CPU `i, j` order -> bit-identical to the old kernel. Guard:
  `test_sycl_psnr_hvs_parity_simd32` (`IGC_ForceOCLSIMDWidth=32`,
  `NEO_CACHE_PERSISTENT=0`). On rebase: no private `int[64]` / `float[64]` in
  this kernel; do not "fix" by pinning `VMAF_SYCL_REQD_SG_SIZE(16)` instead.

- **Tile loaders clamp after reflecting (`sycl_tile_index.h`)**
  (T-SYCL-TILE-HALO-OOB-READ-2026-09-29, Research-2123). Fixed-size SLM tiles
  load padding lanes too; one reflection of those leaves the plane on small
  frames (ADM vertical DWT: row 16 of an 8-row plane -> -1) and reads outside
  the USM buffer -> `UR_RESULT_ERROR_DEVICE_LOST` when the page is unmapped.
  Every single-reflection loader wraps the reflected index in
  `vmaf_sycl_tile_index()`: `integer_adm` `launch_dwt_vert_pair`, `integer_vif`
  `dev_vert_load_tile` + `dev_fused_load_tile`, `integer_motion_pipeline`
  `load_diff` (motion + motion_v2), `float_motion`
  `fm_load_tile`, `float_vif` `load_vif_tile`. Identity for consumed samples ->
  no score change. New tiled kernel = same wrap. Per-output reflections
  (`dev_hori_convolve_border`, float VIF decimate) are consumed-only and stay
  unwrapped; each extractor's minimum frame size keeps them in the plane
  (integer VIF: `VIF_MIN_DIM` above). Guards: `test_sycl_adm_tiny_frames`,
  `test_sycl_vif_min_dim`.

- **`integer_psnr_hvs_sycl.cpp` uses ceiling division for chroma plane
  geometry** (PR #1031). `init_fex_sycl` computes 4:2:0 / 4:2:2 chroma
  `width[1..2]` / `height[1..2]` via `(w + 1U) >> 1` / `(h + 1U) >> 1`, not
  `w >> 1` / `h >> 1`, to match `picture.c` / CPU `integer_psnr_hvs.c` /
  CUDA + HIP twins on odd-dimension YUV420 / YUV422. Floor division drops
  last chroma 8x8 block strip on odd dimensions, diverging `psnr_hvs_cb` /
  `psnr_hvs_cr` / `psnr_hvs` from every other backend (even dimensions
  unaffected). On rebase: picture allocator's ceiling subsample convention
  (`(dim + ss) >> ss`) = single source of truth. Any new SYCL extractor
  re-deriving plane dims in its own `init` must use ceiling form. Any
  upstream change to chroma-dimension formula propagates here and to
  CUDA + HIP twins in same PR.

- **`integer_ms_ssim_sycl.cpp` honours `enable_chroma` option parity**
  (ADR-0526, ADR-0583). `enable_chroma`
  option (default `false`) clamps `n_planes` to 1 in `init_fex_sycl` when
  set to `false`, to 3 otherwise (except YUV400P which always forces 1).
  Chroma geometry uses the picture allocator's ceil subsampling, so a
  176x176 chroma minimum maps to an exact 351x351 4:2:0 luma minimum;
  the init error suggestion uses that exact inverse. Submit, computation and
  publication iterate every active plane, so `enable_chroma=true` dispatches
  Y, Cb and Cr today. On rebase: keep the default, YUV400P clamp and
  three-plane dispatch aligned with the CPU and Metal MS-SSIM extractors.

- **`integer_ms_ssim_sycl.cpp` honours `enable_lcs`, `enable_db`,
  `clip_db` GPU option parity** (ADR-0243, ADR-1078). When
  `enable_lcs=true`, emits 15 extra metrics
  (`float_ms_ssim_{l,c,s}_scale{0..4}`). When `enable_db=true`,
  returns `-10*log10(1 - ms_ssim)` instead of raw linear score;
  `clip_db=true` derives the geometry-dependent `max_db` ceiling from frame
  dimensions and bit depth, then caps the dB-domain output at that ceiling
  (ADR-1221). It never clamps the linear score to `[0, 1]`.
  All three options default to `false` — output at default settings
  numerically identical to pre-ADR-1078 binary. Metric ordering
  and `places=4` cross-backend contract = part of public API
  surface. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU contract"](../../AGENTS.md).

- **`integer_ssim_sycl.cpp` and `integer_ms_ssim_sycl.cpp` are
  self-contained submit/collect** — do **not** register with
  `vmaf_sycl_graph_register`. `integer_ms_ssim_sycl.cpp` needs float
  [0, 255] intermediates from `picture_copy()`; `float_ssim_sycl` uploads
  its own raw luma and does that scaling on the device (ADR-1370).
  `ciede_sycl` TU follows same pattern. **On rebase**: do not
  "consolidate" these into graph register — precision posture
  load-bearing. Reading the shared frame from `float_ssim_sycl` is a
  separate decision (it would also serve `vmaf_read_pictures_sycl()`).

- **`float_ssim_sycl` decimation is bit-exact with the CPU**
  ([ADR-1370](../../../../docs/adr/1370-sycl-float-ssim-device-decimation.md)).
  `decimate_sample()` = `iqa_filter_pixel()` at `(x * scale, y * scale)`:
  window rows / columns `r - scale / 2` for `r` in `[0, scale)`,
  `symmetric_index()` = `KBND_SYMMETRIC`, product `sample * tap_weight`
  in fp32 (`tap_weight` = `ssim.c`'s `1.0f / (scale * scale)`, computed on
  the host), summed exactly in int64 units of 2^-52, converted once with
  `rounding_mode::rte`. No fp32 accumulation, no FMA path (no adds on
  floats), no reordering matters because the sum is integer. Exact only up
  to `SSIM_MAX_EXACT_SCALE` (128); `float_ssim_geometry_supported()` is
  the single predicate for `check_context_sycl()` and init, also requiring
  an 11x11 decimated plane. Plane size = `iqa_decimate_dim()` from
  `iqa/decimate_dim.h` (shared with `iqa/decimate.c`; include-free so this
  C++ TU never parses `convolve.h`). Samples: `picture_copy()`'s layout
  (uint16 at 10 / 12 / 16 bits scaled by the exact reciprocal of 4 / 16 /
  256, else uint8). Frame means go through `float_ssim_frame_mean()`,
  which rounds to fp32 like `iqa_ssim()`: removing it breaks `enable_db`
  on near-identical frames. One queue wait per frame, in `collect()`.
  Guards: `test_sycl_float_ssim_parity` (+ `_large`, scales 1-10, 8 / 10 /
  12-bit, odd sizes, `enable_lcs`, gate verdicts),
  `test_gpu_float_ssim_auto_scale_contract`. On rebase: a change to
  `iqa_filter_pixel()`, `KBND_SYMMETRIC`, `ssim_low_pass_alloc()` or
  `picture_copy()` scaling changes this kernel in the same PR.

- **`integer_ciede_sycl.cpp` stages Y/U/V at native size; kernel
  subsamples chroma** ([Research-2120](../../../../docs/research/2120-sycl-ciede-throughput.md)).
  `stage_plane()` packs each plane into host USM (`plane_w[p]` x
  `plane_h[p]`, chroma by `picture.c`'s ceil rule `(w + ss) >> ss`),
  one DMA per plane. `ciede_pixel()` reads chroma at
  `(x >> ss_hor, y >> ss_ver)` = nearest-neighbour upsample of
  `ciede.c::scale_chroma_planes`: horizontal from `ss_hor`, vertical
  from `ss_ver` (fork's fixed flags, not upstream's transposed pair).
  Same indexing as CUDA / HIP twins. **On rebase**: do not restore the
  host `upscale_plane` (9.5 of 15 ms per 4K frame on Arc B580) and do
  not floor chroma dims. Output bit-identical to the old upscale path;
  `test_sycl_ciede_parity{,_oddw,_422_10b,_444}` pin odd 4:2:0,
  4:2:2 10-bit, 4:4:4 against CPU.

- **`picture_copy()` channel parameter** — `integer_ms_ssim_sycl.cpp`
  passes `channel=0` per d3647c73 prerequisite port
  (`integer_ssim_sycl.cpp` no longer calls `picture_copy()`, ADR-1370). See
  [../../AGENTS.md §"`picture_copy()` carries a `channel`
  parameter"](../../AGENTS.md).

- **`integer_cambi_sycl.cpp` — fully device-resident, graph-registered**
  ([ADR-1357](../../../../docs/adr/1357-sycl-cambi-device-resident.md),
  supersedes the ADR-0415 / ADR-0489 host residual). Reads distorted luma
  from shared frame (`enqueue_fn` `shared_dis`), no own upload. Whole frame
  = one `enqueue_cambi_work`: reset → validate → preprocess → tiled mask →
  per scale {decimate, filter H, filter V + level map Q, `launch_row_masks`,
  `launch_c_values` (+ radix pass 0 + per-group sum), `launch_topk_pooling`}.
  `post_fn` = only D2H (88-byte `CambiSyclResults`); `collect()` = only host
  arithmetic (`vmaf_cambi_weight_scores_per_scale`). Load-bearing:
  - every kernel argument init-time state: graph recording replays
    `enqueue_fn` for both slots; no host decision per frame, no
    `memset`/`fill` inside `enqueue_fn` (reset is a kernel);
  - c-values multiply by `vmaf_cambi_reciprocal_lut()` table, never
    `1.0f / i` (42 entries differ by 1 ulp);
  - top-K sum exact: 128-bit fixed point, units 2^-24 (all non-zero
    c-values in [0.5, 2^14)); no fp64, no float accumulation;
  - `check_window_fits_lut` = `cambi.c::setup_contrast_and_luminance`
    guard, same place (after TVI), same -EINVAL + message, both enc and
    source windows; LUT uploaded verbatim, never extended —
    `test_sycl_cambi_parity` window cases pin accept/reject set;
  - `CambiSyclSelect::k_rem[p + 1]` written by scan of pass p: no lane
    rewrites word another lane reads; bin 0 of pass 0 = exact zeros
    → `resolved`, later passes return on device;
  - histogram cells `uint16`, modular; order of updates free (true window
    counts), so run/skip rewrites keep bit-exactness.
  **On rebase**: `cambi.c` change to `c_value_pixel`,
  `calculate_c_values` window walk, `spatial_pooling`, preprocessing or
  `filter_mode` → mirror into device kernels same PR;
  `test_sycl_cambi_parity` asserts bit-exact per frame.

- **Per-step `q.wait()` in feature extractors forbidden — use
  in-order queue** (ADR-0458 / SY-1). SYCL in-order queue serialises
  all submitted operations automatically; adding `q.wait()` between GPU
  kernels drains queue to idle, prevents pipelining. Only
  mandatory `q.wait()` calls at **CPU-reads-from-device boundaries**
  (i.e., right before host code reads `vmaf_sycl_malloc_host` buffer
  written by preceding `q.memcpy`). Example: `integer_cambi_sycl.cpp`
  has none of its own — `collect()` reads its readback after
  `vmaf_sycl_graph_wait()` (ADR-1357).

- **Stencil/convolution SYCL kernels MUST use `local_accessor` for tap
  reuse** (ADR-0458 / SY-2). Separable filter (Gaussian, box,
  motion-blur) with more than 3 taps **must** stage required input
  region into shared local memory (SLM) via
  `local_accessor` + cooperative tile-load loop + barrier — follow
  pattern in `float_vif_sycl.cpp`, `float_motion_sycl.cpp`, and (post
  ADR-0458) `integer_ssim_sycl.cpp`.
  Bare `parallel_for<range<N>>` reading global memory for every tap =
  lint violation for convolution kernels — use `nd_range` instead.

- **`integer_adm_sycl.cpp` / `float_adm_sycl.cpp` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with same defaults as CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, SYCL twins must update
  in same PR.

- **`motion_fps_weight` cross-backend parity** — see canonical
  invariant note in [`../cuda/AGENTS.md`](../cuda/AGENTS.md).
  `integer_motion_v2_sycl.cpp` and `float_motion_sycl.cpp` both carry
  `motion_fps_weight` option, apply it in `flush()` /
  `collect()` exactly as documented there. Any future change to
  weight application math must span all motion-family GPU twins in
  same PR. Since ADR-1365 `float_motion_sycl.cpp` spells it the CPU
  way, `motion_clip(min(prev, cur))` = min, then weight, then
  `motion_max_val` cap; for weight >= 0 bit-identical to
  weight-before-min the note describes. v1 `integer_motion_sycl.cpp`
  twin covered by same canonical note's **applied exactly once**
  clause (ADR-1216): `motion3_postprocess_sycl()` must not re-apply
  weight its callers already applied.

- **`float_vif_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1217) — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). Compute kernel captures
  `vif_sigma_nsq`, `vif_enhn_gain_limit` and host-derived
  `sigma_max_inv` from `launch_compute`'s parameters; must not
  re-declare them as kernel-local constants.
- **SpEED singular-covariance contract** — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). Since ADR-1358 the SYCL
  twins decide singularity on the device: `linalg_store()` in
  `speed_sycl_pipeline.cpp` writes the per-channel flag, and
  `block_statistics()` solves into a zero-initialised private solution
  that stays zero on a singular channel, so no device buffer is read
  before it is written. `score_group()` applies the one-sided rule and
  the flags reach the host in `FrameResult.singular`. ADR-1218.
- **`float_adm_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1220) — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). `launch_csf_cm` and
  `launch_aim_cm` capture `adm_p_norm`; host pooling uses
  `1.0f / adm_p_norm` for root and noise constant. `adm_bypass_cm`
  (`bcm`) captured into `FadmCmParams.bypass_cm`; `fadm_cm_threshold`
  returns 0.0f when non-zero (CPU/CUDA/Metal parity, ADR-1220).

- **VAAPI / dmabuf zero-copy import** — FFmpeg `libvmaf_sycl`
  filter (`ffmpeg-patches/0005-*.patch`) consumes
  `vmaf_sycl_import_va_surface`. Public-surface change touches
  patch file too — see CLAUDE.md §12 r14 +
  [ADR-0183](../../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md).

- **`ssimulacra2_sycl.cpp` IIR recurrence has no running accumulator —**
  **never 'Kahan' it** (ADR-0985). Charalampidis recursive blur = 3-pole
  autoregressive IIR filter
  ($o_k = n2 \cdot \text{sum} - d1 \cdot \text{prev1} - \text{prev2}$), not
  cumulative summation. Adding accumulator term $\text{prev1}$ into
  output shifts poles outside unit circle ($1 - d1 \approx -0.8422$),
  causing geometric pole blow-up to $10^{25}$ / NaN / saturation at 100.0.
  Recurrence must remain pure float32 matching CUDA twin
  `core/src/feature/cuda/ssimulacra2/ssimulacra2_blur.cu`. Device-level
  fp64-less divergence on Arc A380 calibrated via
  `scripts/ci/gpu_ulp_calibration.yaml` at places=1 (`5.0e-2`), not compensated
  via pseudo-Kahan recurrence.

## icpx-aware clang-tidy

Stock LLVM `clang-tidy` cannot resolve `<sycl/sycl.hpp>`. Use
[`scripts/ci/clang-tidy-sycl.sh`](../../../../scripts/ci/clang-tidy-sycl.sh),
which injects oneAPI SYCL include path +
`-D__SYCL_DEVICE_ONLY__=0`, locates `icpx` via `$ICPX_ROOT` (or
`/opt/intel/oneapi/compiler/latest`). CI lane
`Tidy SYCL` runs wrapper. Required check since ADR-1297;
no longer advisory, no `continue-on-error`.
Adding new SYCL TU needs no AGENTS.md update — wrapper
finds it via changed-file diff. See
[ADR-0217](../../../../docs/adr/0217-sycl-toolchain-cleanup.md).

## Build

SYCL feature TUs compile only when `meson setup -Denable_sycl=true`.
Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.

## Governing ADRs

- [ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches. Every SYCL feature kernel here = row
  in one of these.
- [ADR-0202](../../../../docs/adr/0202-float-adm-cuda-sycl.md) +
  [ADR-0206](../../../../docs/adr/0206-ssimulacra2-cuda-sycl.md) —
  CUDA + SYCL ports pinning `-fp-model=precise` as load-bearing.
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate.
- [ADR-0217](../../../../docs/adr/0217-sycl-toolchain-cleanup.md) —
  icpx-aware clang-tidy wrapper.
- [ADR-0219](../../../../docs/adr/0219-motion3-gpu-contract.md) —
  motion3 GPU contract.
- [ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels unconditionally fp64-free (T7-17).
- [ADR-0243](../../../../docs/adr/0243-enable-lcs-gpu.md) — MS-SSIM
  `enable_lcs` GPU contract.
- [ADR-0985](../../../../docs/adr/0985-sycl-parity-divergence-2026-06-03.md) —
  SYCL SSIMULACRA 2 parity divergence and recurrence resolution.

## Per-kernel parity-test invariant (rounds 1–3)

Every SYCL feature kernel here has a scalar reference and
`core/test/test_sycl_<kernel>_parity.c` gate. Most use ADR-0214 places=4
(1e-4) tolerance; `motion_add_uv` uses ADR-1326's exact fixed-point oracle
because its CPU float semantic twin has different arithmetic. Coverage matrix
below tracks which SYCL kernel maps to which CPU twin and which parity test.
**On rebase**: if SYCL kernel renamed or new one added, parity test name +
ADR-0884 / ADR-0946 backlog must update in same PR.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_cambi_sycl.cpp` | `cambi.c` | `test_sycl_cambi_parity.c` (bit-exact, 4 frames), `test_integer_cambi_sycl.c` (smoke) | [ADR-1357](../../../../docs/adr/1357-sycl-cambi-device-resident.md) |
| `integer_motion_sycl.cpp` (motion3) | `integer_motion.c` | `test_sycl_motion3_parity.c` | ADR-0219 |
| `integer_motion_pipeline_sycl.cpp` (motion + motion_v2 SAD) | `integer_motion.c`, `integer_motion_v2.c` | `test_sycl_motion_tiny_frames.c` (bit-exact, 3x3 .. 1283x723) | T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29 |
| `integer_motion_sycl.cpp` (motion_add_uv) | `float_motion.c` | `test_sycl_motion_add_uv_parity.c` | ADR-0989 |
| `integer_psnr_sycl.cpp` | `integer_psnr.c` | `test_sycl_psnr_parity.c` | ADR-0868 (round 1) |
| `integer_vif_sycl.cpp` | `integer_vif.c` | `test_sycl_vif_parity.c` | ADR-0868 (round 1) |
| `integer_adm_sycl.cpp` | `integer_adm.c` | `test_sycl_adm_parity.c` | ADR-0884 (round 2) |
| `integer_ciede_sycl.cpp` | `ciede.c` | `test_sycl_ciede_parity.c` | ADR-0884 (round 2) |
| `integer_ssim_sycl.cpp` | `integer_ssim.c` | `test_sycl_ssim_parity.c` | ADR-0884 (round 2) |
| `integer_ssim_sycl.cpp` (`float_ssim_sycl`) | `float_ssim.c` + `ssim.c` | `test_sycl_float_ssim_parity.c` (+ `_large`) | ADR-1370 |
| `integer_ms_ssim_sycl.cpp` | `ms_ssim.c` | `test_sycl_ms_ssim_parity.c` | ADR-0884 (round 2) |
| `integer_motion_v2_sycl.cpp` | `integer_motion_v2.c` | `test_sycl_motion_v2_parity.c` | ADR-0884 (round 2) |
| `float_psnr_sycl.cpp` | `float_psnr.c` | `test_sycl_float_psnr_parity.c` | ADR-0946 (round 3) |
| `float_adm_sycl.cpp` | `float_adm.c` | `test_sycl_float_adm_parity.c` | ADR-0946 (round 3) |
| `float_vif_sycl.cpp` | `float_vif.c` | `test_sycl_float_vif_parity.c` | ADR-0946 (round 3) |
| `float_motion_sycl.cpp` | `float_motion.c` | `test_sycl_float_motion_parity.c` | ADR-0946 (round 3) |
| `integer_psnr_hvs_sycl.cpp` | `third_party/xiph/psnr_hvs.c` | `test_sycl_psnr_hvs_parity.c` | ADR-0946 (round 3) |
| `integer_moment_sycl.cpp` (`float_moment_sycl`) | `float_moment.c` | `test_sycl_float_moment_parity.c` | ADR-0957 (round 4) |
| `speed_chroma_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_chroma_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |
| `speed_temporal_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_temporal_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |
| `ssimulacra2_sycl.cpp` | `ssimulacra2.c` | `test_sycl_ssimulacra2_parity.c` | ADR-0957 (round 4) |

> **SpEED twins are wired and device-resident (ADR-0964, ADR-1358).**
> Both extractors are in `sycl_feature_sources` with the shared
> `speed_sycl_pipeline.cpp` and `speed_sycl_host.cpp`. Their parity tests
> are live gates; on real video the twins match the CPU bit for bit (see
> `docs/metrics/speed_qa.md`).

## Per-feature option-table sync invariant

**Adding feature knob to any one backend (SYCL / CUDA / HIP / Metal /
Vulkan) requires adding it to all backends in same PR** — no deferred
follow-ups. Canonical source of truth for option signature (name,
alias, type, min, max, default, flags) = CPU feature extractor in
`core/src/feature/` (e.g. `integer_motion.c`). GPU twins copy
option entry verbatim, apply weight in equivalent host-side
`flush()` or post-processing callback.

Rationale: CHUG / K150K extractor whitelist in
`ai/scripts/extract_k150k_features.py` passes `_feature_arg` dicts to
`vmaf_use_features_with_opts`; if receiving backend's options table
misses knob, option silently falls through to default,
producing silently-wrong scores without any error. Root cause
of `motion_fps_weight` gap in `integer_motion_v2_sycl.cpp`, closed by
PR #851-follow-up (2026-05-16).

Second instance (ADR-1179, `fix/sycl-v1-model-crash`): `options_cambi_sycl`
lacked `cambi_high_res_speedup` (`hrs`). That knob carries
`VMAF_OPT_FLAG_FEATURE_PARAM`; its absence changed *serialised feature
name* — SYCL twin emitted `cambi_cmxv_17_vlt_0.06` while default
model `vmaf_v1.0.16_3d0h` asks for `cambi_hrs_1080_cmxv_17_vlt_0.06` —
prediction failed with `-EAGAIN` instead of falling through to default.
Two rebase-sensitive consequences: (1) every `VMAF_OPT_FLAG_FEATURE_PARAM`
knob of `cambi.c` must exist verbatim in `options_cambi_sycl`; (2)
`vmaf_feature_name_dict_from_provided_features()` must run in
`init_fex_sycl` **before** `enc_width` / `enc_height` / `enc_bitdepth`
default from picture geometry (same ordering as `cambi.c`),
else geometry defaults leak into feature name. TVI / VLT
tables come from shared `vmaf_cambi_init_tvi_and_vlt()` in `cambi.c`
— do not reintroduce private bisection in twin.

## Per-kernel parity-test invariant (ADR-0214 + ADR-0868 + ADR-0884)

**Every shipping SYCL kernel here must have a scalar-vs-SYCL parity test
under [`core/test/`](../../../test/), wired into
[`core/test/meson.build`](../../../test/meson.build) with suite
`['fast', 'gpu']`.** A parity test normally asserts the headline score
matches its CPU scalar reference within ADR-0214 places=4 (`1e-4`);
`motion_add_uv` instead matches an arithmetic-identical fixed-point oracle
within ADR-1326's derived host-double bound. Tests skip cleanly when no SYCL
device visible — mirrors `[skip: no SYCL device]` pattern in
[`test_sycl_motion3_parity.c`](../../../test/test_sycl_motion3_parity.c).

Coverage matrix:

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_psnr_sycl.cpp` | `test_sycl_psnr_parity.c` | [ADR-0868](../../../../docs/adr/0868-gpu-backend-kernel-coverage.md) |
| `integer_vif_sycl.cpp` | `test_sycl_vif_parity.c` | ADR-0868 |
| `integer_adm_sycl.cpp` | `test_sycl_adm_parity.c` | [ADR-0884](../../../../docs/adr/0884-sycl-kernel-coverage-round2.md) |
| `integer_ciede_sycl.cpp` | `test_sycl_ciede_parity.c` | ADR-0884 |
| `integer_ssim_sycl.cpp` (integer fex) | `test_sycl_ssim_parity.c` | ADR-0884 |
| `integer_ms_ssim_sycl.cpp` | `test_sycl_ms_ssim_parity.c` | ADR-0884 |
| `integer_motion_v2_sycl.cpp` | `test_sycl_motion_v2_parity.c` | ADR-0884 |
| `integer_motion_sycl.cpp` | `test_sycl_motion3_parity.c` | [ADR-0219](../../../../docs/adr/0219-motion3-gpu-contract.md) |
| `integer_motion_sycl.cpp` (motion_add_uv) | `test_sycl_motion_add_uv_parity.c` | [ADR-0989](../../../../docs/adr/0989-sycl-motion-add-uv.md) |
| `integer_cambi_sycl.cpp` | `test_sycl_cambi_parity.c` (bit-exact per frame) + `test_integer_cambi_sycl.c` (smoke) | [ADR-0415](../../../../docs/adr/0415-cambi-sycl-port.md), [ADR-1357](../../../../docs/adr/1357-sycl-cambi-device-resident.md) |
| `float_*_sycl.cpp`, `speed_*_sycl.cpp`, `ssimulacra2_sycl.cpp`, `integer_moment_sycl.cpp`, `integer_psnr_hvs_sycl.cpp` | (round 3 backlog — see ADR-0884) | — |

**Rebase-sensitive**: adding new SYCL kernel TU, same PR
must add matching `test_sycl_<kernel>_parity.c` and meson
wiring. `/cross-backend-diff` skill = dev-time tool only,
does NOT run in CI on every PR; only in-tree repository-runner parity
tests catch per-kernel regressions automatically.

## motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_sycl` emits `motion3_v2_score` host-side in
  flush, mirroring CPU `integer_motion_v2.c::flush` and CUDA twin
  byte-for-byte: per-frame `motion_blend(motion2, blend_factor,
  blend_offset)` then `MIN(_, motion_max_val)` clip, a `stamp_value` seed
  for `i < min_idx (= 1)`, and optional 2-tap `motion_moving_average`,
  via shared `motion_blend_tools.h` helper. Any change to CPU
  flush blend/clip/seed/average logic must mirror into all four GPU
  twins (cuda/sycl/hip/metal) in same PR to keep `places=4`
  `test_sycl_motion_v2_parity` gate green.

## Integer ADM tiny frames and linkage (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_sycl()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- `integer_adm_sycl.cpp` internals live in one anonymous namespace; only the
  `extern "C"` extractor struct has external linkage. No C-style `static` at
  file scope, no linkage NOLINT band.
- Scale 0 = CPU int16 semantics (T-SYCL-ADM-INT16-SEMANTICS-2026-09-18). CPU
  stores bands as `int16_t`; kernels compute wider, so wrap explicitly with
  `adm_i16()` (mod 2^16, compiler-independent) wherever the CPU narrows:
  `csf_a`, `csf_f`, CM 1/15 centre tap. Centre tap = the one default weights
  reach (`|csf_a| >= 15360`). Diagonal `csf_a` rounds with 65535, not
  `1 << 16`. Scale-0 CM measure is int32, mod 2^32. Never widen these.
- Scales 1-3 `>> 32` rounding term = `I4_FLT_ROUND` = -2^31: the CPU's
  wrapped `(int32_t)(1u << 31)` (Netflix#955, ADR-0155), same as CUDA / HIP.
  Netflix fixes #955 -> change CPU and this constant together.
- Host finalisation = the CPU's float arithmetic (ADR-1362) -> every ADM
  output bit-exact. The old double finaliser (`conclude_adm_cm` /
  `conclude_adm_csf_den`, ~1e-7 residual) is gone; do not bring it back.
- Guard: `test_sycl_adm_tiny_frames` (tiny frames + full-range noise).

## Integer ADM AIM pass (ADR-1362, T-GPU-ADM-AIM-DEVICE-PASS-MISSING-SYCL-HIP-2026-09-05)

- `adm_sycl` provides `VMAF_integer_feature_aim_score` +
  `VMAF_integer_feature_adm3_score` -> default model `vmaf_v1.0.16_3d0h` runs
  its whole ADM branch on the device. AIM = CPU `measure_aim`: threshold from
  csf(r) (3x3 `|csf(r)| / 30` + 1/15 centre), measure t - r, no noise floor.
- Per scale 2 kernels after the DWT: `launch_decouple_csf` stores
  `d_csf_f` (`|csf(t - r)| / 30`) + `d_csf_f_aim` (`|csf(r)| / 30`) over the
  whole band; `launch_csf_den_cm` = one work-group per region row, all 3
  bands, 9 sums (CM, CSF den, AIM). r and t - r recomputed per sample
  (`adm_dev_sample`); storing r measured slower on the UHD 770 (bandwidth).
- One accumulator buffer `[term][scale][band]` (`adm_accum_slot`, 36 int64):
  one memset in `pre_fn`, one D2H in `post_fn`, read in `collect()` after
  `vmaf_sycl_graph_wait()`. No host wait inside a frame; keep it that way.
- Row fold through `adm_cm_round_row_total()` in `adm_dev_fold_row`, once per
  row per sum (ADR-1167). `test_adm_cm_row_rounding_contract.py` pins it.
- `adm_dev_decouple_k` clamps the Q15 quotient in int64 like the CPU's
  `tmp_k`. Old kernel narrowed to int32 first -> |t / o| > 2^16 at scales 1-3
  wrapped -> `integer_adm_scale2` up to 1.40e-6 off the CPU at 4K, aim not
  exact. Never
  narrow before the clamp.
- All outputs (adm2, scale*, debug num / den, aim, adm3) finalised in the
  CPU's own float arithmetic (`adm_cm_scale_cpu`, `adm_den_scale_cpu`,
  `adm_scale_cpu`, `adm_terms`, `adm_finalise`: float per band, float per
  scale, double sum, `(float)1e-10` skip-scale0 den, floor in place) ->
  bit-exact. Maintainer contract: bit-exact with the CPU. Any double
  shortcut here breaks `test_sycl_adm_parity` / `test_sycl_adm_tiny_frames`.
- CM kernel sub-group size 16: 9 int64 sums spill at 32 lanes on Xe-LP.
- `adm_skip_aim` mirrors the CPU: AIM sums skipped, aim = 0.
- Non-integer `adm_enhn_gain_limit` (e.g. 1.2): Q31 floor emulation != CPU
  trunc(double) -> aim / adm3 ~1.4e-7 off, as adm2. Integer gains (1.0, 100.0,
  every shipped model) exact.
- Guards: `test_sycl_adm_parity` (`test_adm_cpu_sycl_aim_bit_exact`),
  `test_sycl_adm_tiny_frames` (aim / adm3 bit-exact under `HAVE_SYCL`: tiny,
  noise, 16-bit, CSF modes 0-3), `python/test/gpu_default_model_test.py`.
