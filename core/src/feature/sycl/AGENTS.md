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
  (ADR-0206). Matches GLSL `precise` / `NoContraction` and CUDA
  `--fmad=false`.
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
  `speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` use role-prefixed
  `launch_{chroma,temporal}_{indterm,score}` names. Their anonymous kernel
  lambdas otherwise receive identical generated names across translation
  units, allowing the linker to pair one launcher's host capture layout with
  the other launcher's device image. Never collapse the role prefixes.
  `float_psnr_sycl.cpp` and `integer_psnr_sycl.cpp` capture their output
  pointers through `FpsnrOutput` and `PsnrKernelArgs`; do not flatten those
  structs back into raw lambda captures. `integer_moment_sycl.cpp` is the
  remaining scalar-argument shape and aliases `d_sums` to `e_sums` before the
  submit lambda. Keep the alias and use it for all four atomics. The source
  contract in `core/test/test_sycl_kernel_source_contract.py` plants the fp64,
  cross-TU kernel-name, and raw-capture regressions and must stay wired into
  the fast suite.
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

- **`integer_motion_sycl.cpp::motion3_postprocess_*` honours
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state.
  `motion_five_frame_window=true` returns `-ENOTSUP` at `init()` with
  `WARNING` log. See [../../AGENTS.md §"motion3_score GPU contract"](../../AGENTS.md).

- **`integer_motion_sycl.cpp::motion_add_uv` GPU contract** (ADR-0989).
  When `motion_add_uv=true`, `submit_fex_sycl` uploads U and V plane data
  H2D to `d_ref_u[cur_blur]` / `d_ref_v[cur_blur]` before calling
  `vmaf_sycl_graph_submit`. `enqueue_motion_work` launches additional
  `launch_blur_sad_fused` kernels for U and V, each writing to
  `d_blur_u/v[cur]`, accumulating into `d_sad_u` / `d_sad_v`.
  `collect_fex_sycl` sums Y + U + V contributions, each normalized by
  respective plane area (`chroma_w × chroma_h` for UV in YUV420P),
  matching `float_motion(motion_add_uv=true)` CPU parity at places=4.
  CUDA, Vulkan, HIP, and Metal twins expose option but return
  `-ENOTSUP` with `WARNING` until their kernel ports land. On rebase:
  if upstream Netflix adds `motion_add_uv` to `integer_motion.c`, verify
  per-plane normalization formula stays consistent.
  **Queue-sync invariant (ADR-1034)**: `vmaf_sycl_memcpy_h2d_async` submits
  UV H2D copies to `state->queue` (primary queue), NOT same as
  `copy_queue` (DMA engine used for Y-plane uploads). `vmaf_sycl_graph_submit`
  barriers `combined_queue` only on `last_upload_event` from `copy_queue`.
  So `submit_fex_sycl` calls `vmaf_sycl_queue_wait(state)` after UV copies
  to flush primary queue before graph submission. If future PR routes UV H2D
  through `copy_queue` and updates `last_upload_event`, `vmaf_sycl_queue_wait`
  call can be removed in favour of GPU-side barrier — update this note then.

- **`integer_vif_sycl.cpp` rd_stride uses ceiling division for odd widths** (ADR-1034).
  Both `launch_vif_hori_impl` (scalar/SIMD-32) and `launch_vif_fused_impl` (SIMD-16)
  compute downsampled row stride as `(e_w + 1U) / 2U`, not `e_w / 2U`.
  `rd_ref`/`rd_dis` allocation in `init_fex_sycl` uses `((w+1U)/2U) * ((h+1U)/2U)`
  elements. Must stay in sync. On rebase: if future PR modifies
  downsampling path, ensure all three sites (two kernel variants + allocation) use
  same ceiling formula. For even widths/heights result identical to
  truncating division.

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

- **`integer_psnr_sycl.cpp` uses ceiling division for chroma plane geometry**
  (PR #878 Vulkan twin fix). `cw` and `ch` computed via
  `(w + 1U) >> 1` / `(h + 1U) >> 1`, not `w / 2U` / `h / 2U`, to match
  CPU + CUDA + Vulkan behaviour on odd-dimension YUV420. On rebase: if
  upstream Netflix changes chroma-dimension formula in
  `integer_psnr.c::init`, propagate here and to CUDA and Vulkan twins
  in same PR.

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
  (mirrors ms_ssim_vulkan PR #957 / ADR-0453 pattern). `enable_chroma`
  option (default `false`) clamps `n_planes` to 1 in `init_fex_sycl` when
  set to `false`, to 3 otherwise (except YUV400P which always forces 1).
  v1 kernel reads plane 0 only; `n_planes > 1` reserved for v2. On rebase:
  keep default `false` and clamp logic aligned with Vulkan and CUDA
  MS-SSIM twins; all three backends must agree on default and dispatch.

- **`integer_ms_ssim_sycl.cpp` honours `enable_lcs`, `enable_db`,
  `clip_db` GPU option parity** (ADR-0243, ADR-1078). When
  `enable_lcs=true`, emits 15 extra metrics
  (`float_ms_ssim_{l,c,s}_scale{0..4}`). When `enable_db=true`,
  returns `-10*log10(1 - ms_ssim)` instead of raw linear score;
  `clip_db` clamps linear value to `[0, 1]` before conversion.
  All three options default to `false` — output at default settings
  numerically identical to pre-ADR-1078 binary. Metric ordering
  and `places=4` cross-backend contract = part of public API
  surface. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU contract"](../../AGENTS.md).

- **`integer_ssim_sycl.cpp` and `integer_ms_ssim_sycl.cpp` are
  self-contained submit/collect** — do **not** register with
  `vmaf_sycl_graph_register` because shared `shared_frame` is
  luma-only packed at uint width, SSIM needs float [0, 255]
  intermediates with `picture_copy()` normalisation. `ciede_sycl`
  TU follows same pattern. **On rebase**: do not "consolidate"
  these into graph register — precision posture
  load-bearing.

- **`picture_copy()` channel parameter** — `integer_ms_ssim_sycl.cpp`
  and `integer_ssim_sycl.cpp` pass `channel=0` per d3647c73
  prerequisite port. See
  [../../AGENTS.md §"`picture_copy()` carries a `channel`
  parameter"](../../AGENTS.md).

- **`integer_cambi_sycl.cpp` — Strategy II hybrid: no graph register,
  event-chained GPU passes with synchronous D2H barrier** (T3-15 /
  ADR-0371 / SY-1 perf fix 2026-05-16). `submit()` flow: H2D
  upload + single `q.wait()` → `launch_spatial_mask` (returns event) →
  per-scale `launch_decimate` image + mask (each returns event, chained
  via `depends_on`) → `launch_filter_mode` H + V (chained via events) →
  `ev_prev.wait()` to drain GPU work → D2H memcpy rows → `q.wait()` →
  `vmaf_cambi_calculate_c_values` + `vmaf_cambi_spatial_pooling`.
  GPU-to-GPU transitions use `sycl::event` chains, not `q.wait()`;
  only H2D-drain and pre-D2H barriers call `wait()`.
  CPU-residual phases must stay inside `submit()`, not `collect()`.
  `collect()` only emits `s->score`. Do **not** move CPU residual
  into `collect()` and do **not** register with `vmaf_sycl_graph_register`
  — per-scale D2H readback and host histogram pass incompatible
  with graph-replay model. CUDA twin (ADR-0360) retains
  synchronous v1 posture; event-chain refactor SYCL-only.

- **Per-step `q.wait()` in feature extractors forbidden — use
  in-order queue** (ADR-0458 / SY-1). SYCL in-order queue serialises
  all submitted operations automatically; adding `q.wait()` between GPU
  kernels drains queue to idle, prevents pipelining. Only
  mandatory `q.wait()` calls at **CPU-reads-from-device boundaries**
  (i.e., right before host code reads `vmaf_sycl_malloc_host` buffer
  written by preceding `q.memcpy`). Example: `integer_cambi_sycl.cpp`
  has exactly one `q.wait()` per scale, right before
  `vmaf_cambi_calculate_c_values`.

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
  same PR. v1 `integer_motion_sycl.cpp` twin covered by
  same canonical note's **applied exactly once** clause (ADR-1216):
  `motion3_postprocess_sycl()` must not re-apply weight its callers
  already applied.

- **`float_vif_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1217) — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). Compute kernel captures
  `vif_sigma_nsq`, `vif_enhn_gain_limit` and host-derived
  `sigma_max_inv` from `launch_compute`'s parameters; must not
  re-declare them as kernel-local constants.
- **SpEED singular-covariance contract** — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). `speed_chroma_sycl.cpp` and
  `speed_temporal_sycl.cpp` zero `d_sol` with `q.memset`, report via
  `singular_out`. SYCL = worst case for getting this wrong:
  `sycl::malloc_device` memory explicitly uninitialised, so missing
  device zero = genuine uninitialised read on first singular
  frame. ADR-1218.
- **`float_adm_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1220) — see canonical note in
  [`../cuda/AGENTS.md`](../cuda/AGENTS.md). `launch_csf_cm` and
  `launch_aim_cm` capture `adm_p_norm`; host pooling uses
  `1.0f / adm_p_norm` for root and noise constant. This twin
  does not declare `adm_bypass_cm`, rejects it — deliberate,
  adding it = feature, tracked in `docs/state.md`.

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

Every SYCL feature kernel here has CPU twin and
`core/test/test_sycl_<kernel>_parity.c` gate at ADR-0214 places=4
(1e-4) tolerance. Coverage matrix below tracks which SYCL kernel
maps to which CPU twin and which parity test. **On rebase**: if
SYCL kernel renamed or new one added, parity test name +
ADR-0884 / ADR-0946 backlog must update in same PR.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_cambi_sycl.cpp` | `cambi.c` | `test_integer_cambi_sycl.c` | pre-existing |
| `integer_motion_sycl.cpp` (motion3) | `integer_motion.c` | `test_sycl_motion3_parity.c` | ADR-0219 |
| `integer_motion_sycl.cpp` (motion_add_uv) | `float_motion.c` | `test_sycl_motion_add_uv_parity.c` | ADR-0989 |
| `integer_psnr_sycl.cpp` | `integer_psnr.c` | `test_sycl_psnr_parity.c` | ADR-0868 (round 1) |
| `integer_vif_sycl.cpp` | `integer_vif.c` | `test_sycl_vif_parity.c` | ADR-0868 (round 1) |
| `integer_adm_sycl.cpp` | `integer_adm.c` | `test_sycl_adm_parity.c` | ADR-0884 (round 2) |
| `integer_ciede_sycl.cpp` | `ciede.c` | `test_sycl_ciede_parity.c` | ADR-0884 (round 2) |
| `integer_ssim_sycl.cpp` | `integer_ssim.c` | `test_sycl_ssim_parity.c` | ADR-0884 (round 2) |
| `integer_ms_ssim_sycl.cpp` | `ms_ssim.c` | `test_sycl_ms_ssim_parity.c` | ADR-0884 (round 2) |
| `integer_motion_v2_sycl.cpp` | `integer_motion_v2.c` | `test_sycl_motion_v2_parity.c` | ADR-0884 (round 2) |
| `float_psnr_sycl.cpp` | `float_psnr.c` | `test_sycl_float_psnr_parity.c` | ADR-0946 (round 3) |
| `float_adm_sycl.cpp` | `float_adm.c` | `test_sycl_float_adm_parity.c` | ADR-0946 (round 3) |
| `float_vif_sycl.cpp` | `float_vif.c` | `test_sycl_float_vif_parity.c` | ADR-0946 (round 3) |
| `float_motion_sycl.cpp` | `float_motion.c` | `test_sycl_float_motion_parity.c` | ADR-0946 (round 3) |
| `integer_psnr_hvs_sycl.cpp` | `third_party/xiph/psnr_hvs.c` | `test_sycl_psnr_hvs_parity.c` | ADR-0946 (round 3) |
| `integer_moment_sycl.cpp` (`float_moment_sycl`) | `float_moment.c` | `test_sycl_float_moment_parity.c` | ADR-0957 (round 4) |
| `speed_chroma_sycl.cpp` (dormant — not built) | `speed.c` | `test_sycl_speed_chroma_parity.c` (skips until wired in) | ADR-0957 (round 4) |
| `speed_temporal_sycl.cpp` (dormant — not built) | `speed.c` | `test_sycl_speed_temporal_parity.c` (skips until wired in) | ADR-0957 (round 4) |
| `ssimulacra2_sycl.cpp` | `ssimulacra2.c` | `test_sycl_ssimulacra2_parity.c` | ADR-0957 (round 4) |

> **`speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` dormant
> scaffold (ADR-0957 §Context).** Source files exist (~1.5 KLOC
> combined, no TODO/FIXME markers) but not in
> `sycl_feature_sources` in `core/src/meson.build`; extractor
> symbols `vmaf_fex_speed_chroma_sycl` / `vmaf_fex_speed_temporal_sycl`
> not declared/registered in `core/src/feature/feature_extractor.c`.
> Wiring them in = separate PR — changes production
> extractor surface, not test coverage alone. Round-4 parity
> tests added in dormant form, auto-activate as real gates
> the day wiring lands.

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

**Every shipping SYCL kernel here must have CPU-vs-SYCL parity test
under [`core/test/`](../../../test/), wired into
[`core/test/meson.build`](../../../test/meson.build) with suite
`['fast', 'gpu']`.** Parity test asserts headline score
matches CPU scalar reference within ADR-0214 places=4 (`1e-4`)
tolerance. Skips cleanly when no SYCL device visible — mirrors
`[skip: no SYCL device]` pattern in
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
| `integer_cambi_sycl.cpp` | `test_integer_cambi_sycl.c` (smoke + score sanity) | [ADR-0371](../../../../docs/adr/0371-sycl-cambi-port.md) |
| `float_*_sycl.cpp`, `speed_*_sycl.cpp`, `ssimulacra2_sycl.cpp`, `integer_moment_sycl.cpp`, `integer_psnr_hvs_sycl.cpp` | (round 3 backlog — see ADR-0884) | — |

**Rebase-sensitive**: adding new SYCL kernel TU, same PR
must add matching `test_sycl_<kernel>_parity.c` and meson
wiring. `/cross-backend-diff` skill = dev-time tool only,
does NOT run in CI on every PR; only in-tree `meson test` parity
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
- Residual vs scalar CPU (~1e-7) = double host finalisation. With the CPU's
  float finalisation swapped in, every score matched the CPU exactly (noise
  at five sizes, src01, checkerboards).
- Guard: `test_sycl_adm_tiny_frames` (tiny frames + full-range noise).
