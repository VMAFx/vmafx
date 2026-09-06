<!-- markdownlint-disable MD060 -->
# AGENTS.md — core/src/feature/sycl

Orientation for agents working on per-feature SYCL kernels (DPC++).
Parent: [../AGENTS.md](../AGENTS.md). The backend runtime (queue, USM,
dmabuf import) lives one level up in
[`../../sycl/AGENTS.md`](../../sycl/AGENTS.md).

## Scope

```text
feature/sycl/
  <feature>_sycl.cpp           # one TU per kernel: registration + submit/collect + sycl::queue::submit lambda
```

All TUs are compiled with `icpx` (Intel oneAPI) — the build line
under [`../../meson.build`](../../meson.build) adds
`-fsycl -fp-model=precise` for every per-kernel TU.

## Ground rules

- **Parent rules** apply (see [../AGENTS.md](../AGENTS.md) +
  [../../AGENTS.md](../../AGENTS.md) +
  [`../../sycl/AGENTS.md`](../../sycl/AGENTS.md)).
- **`-fp-model=precise` on the SYCL feature line is load-bearing.**
  Removing it allows `icpx` to FMA-contract inside the kernel
  lambdas, which drifts `float_adm_sycl` past `places=4` at scale 2
  (ADR-0202) and `ssimulacra2_sycl` past `places=2` through the IIR
  (ADR-0206). Matches GLSL `precise` / `NoContraction` and CUDA
  `--fmad=false`.
- **fp64-free kernels are non-negotiable** ([ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md)).
  Every SYCL feature-kernel lambda captures and operates on `float`
  / integer types only. **No `double` operand inside a `parallel_for`
  body**, no `sycl::reduction<double>`, no `sycl::plus<double>`.
  This is hard, not soft: a single fp64 instruction anywhere in the
  TU's SPIR-V module causes the Level Zero runtime to reject the
  entire module on Intel Arc A-series and other fp64-less devices,
  even when the offending kernel is never submitted.
  - `double` is allowed **outside** the kernel lambda — host-side
    post-processing in `extract` / `flush` callbacks, score
    aggregation, log10 normalisation.
  - ADM gain limiting uses int64 Q31 (`gain_limit_to_q31` +
    `launch_decouple_csf<false>` in `integer_adm_sycl.cpp`).
  - VIF gain limiting uses fp32 `sycl::fmin`.
- **Wholly-new fork files use the dual Netflix + Lusoris/Claude
  copyright header** per [ADR-0025](../../../../docs/adr/0025-copyright-handling-dual-notice.md).
  Most TUs in this directory are fork-original SYCL ports of
  Netflix CUDA kernels.

## Twin-update rules

Every SYCL TU in this directory has CUDA + Vulkan twins. The complete
table lives in [`../cuda/AGENTS.md`](../cuda/AGENTS.md); changes to a
SYCL TU **must** ship with matching changes to the CUDA + Vulkan
twins in the **same PR**. The cross-backend parity gate at `places=4`
([`scripts/ci/cross_backend_parity_gate.py`](../../../../scripts/ci/cross_backend_parity_gate.py),
ADR-0214) catches drift but only after a full GPU run.

## Parity invariant — motion3 CPU and SYCL moving-average paths

`integer_motion.c` (CPU) and `integer_motion_sycl.cpp` (SYCL) both implement
the motion3 post-process as a host-side moving average over blended motion2
scores. These two paths **must stay in numerical parity at places=4** (delta
≤ 1e-4, per ADR-0214). The gate is enforced by
`core/test/test_sycl_motion3_parity.c`; any change to the blend formula
(`motion_blend()`), the moving-average guard condition, or `motion_max_val`
clipping must be mirrored across both files (and across the CUDA / Vulkan /
HIP / Metal motion twins listed in the Twin-update table above) in the same PR.

## Rebase-sensitive invariants

- **`integer_motion_sycl.cpp::motion3_postprocess_*` honours the
  motion3 GPU contract** (ADR-0219). Applies CPU's host-side
  post-process to motion2 with no device-side state.
  `motion_five_frame_window=true` returns `-ENOTSUP` at `init()` with
  a `WARNING` log. See [../../AGENTS.md §"motion3_score GPU contract"](../../AGENTS.md).

- **`integer_motion_sycl.cpp::motion_add_uv` GPU contract** (ADR-0989).
  When `motion_add_uv=true`, `submit_fex_sycl` uploads U and V plane data
  H2D to `d_ref_u[cur_blur]` / `d_ref_v[cur_blur]` before calling
  `vmaf_sycl_graph_submit`. `enqueue_motion_work` launches additional
  `launch_blur_sad_fused` kernels for U and V, each writing to
  `d_blur_u/v[cur]` and accumulating into `d_sad_u` / `d_sad_v`.
  `collect_fex_sycl` sums Y + U + V contributions, each normalized by
  the respective plane area (`chroma_w × chroma_h` for UV in YUV420P),
  matching `float_motion(motion_add_uv=true)` CPU parity at places=4.
  The CUDA, Vulkan, HIP, and Metal twins expose the option but return
  `-ENOTSUP` with a `WARNING` until their kernel ports land. On rebase:
  if upstream Netflix adds `motion_add_uv` to `integer_motion.c`, verify
  that the per-plane normalization formula remains consistent.
  **Queue-sync invariant (ADR-1034)**: `vmaf_sycl_memcpy_h2d_async` submits
  UV H2D copies to `state->queue` (primary queue), which is NOT the same as
  `copy_queue` (the DMA engine used for Y-plane uploads). `vmaf_sycl_graph_submit`
  barriers `combined_queue` only on `last_upload_event` from `copy_queue`.
  Therefore `submit_fex_sycl` calls `vmaf_sycl_queue_wait(state)` after UV copies
  to flush the primary queue before graph submission. If a future PR routes UV H2D
  through `copy_queue` and updates `last_upload_event`, the `vmaf_sycl_queue_wait`
  call can be removed in favour of the GPU-side barrier — update this note then.

- **`integer_vif_sycl.cpp` rd_stride uses ceiling division for odd widths** (ADR-1034).
  Both `launch_vif_hori_impl` (scalar/SIMD-32) and `launch_vif_fused_impl` (SIMD-16)
  compute the downsampled row stride as `(e_w + 1U) / 2U`, not `e_w / 2U`. The
  `rd_ref`/`rd_dis` allocation in `init_fex_sycl` uses `((w+1U)/2U) * ((h+1U)/2U)`
  elements. These must stay in sync. On rebase: if a future PR modifies the
  downsampling path, ensure all three sites (two kernel variants + allocation) use
  the same ceiling formula. For even widths/heights the result is identical to
  truncating division.

- **`integer_psnr_sycl.cpp` honours `enable_chroma` option parity**
  (ADR-0453). The `enable_chroma` option (default `true`) clamps `n_planes`
  to 1 in `init_fex_sycl` when set to `false`, matching CPU
  `integer_psnr.c::init`'s behaviour. On rebase: keep the clamp and its
  `default_val.b = true` aligned with the CUDA and Vulkan twins; all three
  backends must agree on the default and the dispatch logic.

- **`integer_psnr_sycl.cpp` uses ceiling division for chroma plane geometry**
  (PR #878 Vulkan twin fix). `cw` and `ch` are computed via
  `(w + 1U) >> 1` / `(h + 1U) >> 1`, not `w / 2U` / `h / 2U`, to match
  CPU + CUDA + Vulkan behaviour on odd-dimension YUV420. On rebase: if
  upstream Netflix changes the chroma-dimension formula in
  `integer_psnr.c::init`, propagate it here and to the CUDA and Vulkan twins
  in the same PR.

- **`integer_psnr_hvs_sycl.cpp` uses ceiling division for chroma plane
  geometry** (PR #1031). `init_fex_sycl` computes the 4:2:0 / 4:2:2 chroma
  `width[1..2]` / `height[1..2]` via `(w + 1U) >> 1` / `(h + 1U) >> 1`, not
  `w >> 1` / `h >> 1`, to match `picture.c` / CPU `integer_psnr_hvs.c` / the
  CUDA + HIP twins on odd-dimension YUV420 / YUV422. Floor division drops the
  last chroma 8x8 block strip on odd dimensions, diverging `psnr_hvs_cb` /
  `psnr_hvs_cr` / `psnr_hvs` from every other backend (even dimensions are
  unaffected). On rebase: the picture allocator's ceiling subsample convention
  (`(dim + ss) >> ss`) is the single source of truth — any new SYCL extractor
  that re-derives plane dims in its own `init` must use the ceiling form, and
  any upstream change to the chroma-dimension formula propagates here and to
  the CUDA + HIP twins in the same PR.

- **`integer_ms_ssim_sycl.cpp` honours `enable_chroma` option parity**
  (mirrors ms_ssim_vulkan PR #957 / ADR-0453 pattern). The `enable_chroma`
  option (default `false`) clamps `n_planes` to 1 in `init_fex_sycl` when
  set to `false`, and to 3 otherwise (except YUV400P which always forces 1).
  v1 kernel reads plane 0 only; `n_planes > 1` is reserved for v2. On rebase:
  keep default `false` and the clamp logic aligned with the Vulkan and CUDA
  MS-SSIM twins; all three backends must agree on the default and dispatch.

- **`integer_ms_ssim_sycl.cpp` honours the `enable_lcs`, `enable_db`,
  and `clip_db` GPU option parity** (ADR-0243, ADR-1078). When
  `enable_lcs=true`, emits 15 extra metrics
  (`float_ms_ssim_{l,c,s}_scale{0..4}`). When `enable_db=true`,
  returns `-10*log10(1 - ms_ssim)` instead of the raw linear score;
  `clip_db` clamps the linear value to `[0, 1]` before conversion.
  All three options default to `false` — output at default settings
  is numerically identical to the pre-ADR-1078 binary.  Metric ordering
  and `places=4` cross-backend contract are part of the public API
  surface. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU contract"](../../AGENTS.md).

- **`integer_ssim_sycl.cpp` and `integer_ms_ssim_sycl.cpp` are
  self-contained submit/collect** — they do **not** register with
  `vmaf_sycl_graph_register` because the shared `shared_frame` is
  luma-only packed at uint width and SSIM needs float [0, 255]
  intermediates with `picture_copy()` normalisation. The `ciede_sycl`
  TU follows the same pattern. **On rebase**: do not "consolidate"
  these into the graph register — the precision posture is
  load-bearing.

- **`picture_copy()` channel parameter** — `integer_ms_ssim_sycl.cpp`
  and `integer_ssim_sycl.cpp` pass `channel=0` per the d3647c73
  prerequisite port. See
  [../../AGENTS.md §"`picture_copy()` carries a `channel`
  parameter"](../../AGENTS.md).

- **`integer_cambi_sycl.cpp` — Strategy II hybrid: no graph register,
  event-chained GPU passes with synchronous D2H barrier** (T3-15 /
  ADR-0371 / SY-1 perf fix 2026-05-16). The `submit()` flow is: H2D
  upload + single `q.wait()` → `launch_spatial_mask` (returns event) →
  per-scale `launch_decimate` image + mask (each returns event, chained
  via `depends_on`) → `launch_filter_mode` H + V (chained via events) →
  `ev_prev.wait()` to drain GPU work → D2H memcpy rows → `q.wait()` →
  `vmaf_cambi_calculate_c_values` + `vmaf_cambi_spatial_pooling`.
  GPU-to-GPU transitions use `sycl::event` chains, not `q.wait()`;
  only the H2D-drain and the pre-D2H barriers call `wait()`.
  The CPU-residual phases must stay inside `submit()`, not `collect()`.
  `collect()` only emits `s->score`. Do **not** move the CPU residual
  into `collect()` and do **not** register with `vmaf_sycl_graph_register`
  — the per-scale D2H readback and host histogram pass are incompatible
  with the graph-replay model. The CUDA twin (ADR-0360) retains
  synchronous v1 posture; the event-chain refactor is SYCL-only.

- **Per-step `q.wait()` in feature extractors is forbidden — use the
  in-order queue** (ADR-0458 / SY-1). The SYCL in-order queue serialises
  all submitted operations automatically; adding `q.wait()` between GPU
  kernels drains the queue to idle and prevents pipelining. The only
  mandatory `q.wait()` calls are at **CPU-reads-from-device boundaries**
  (i.e., right before host code reads a `vmaf_sycl_malloc_host` buffer
  that was written by a preceding `q.memcpy`). Example: `integer_cambi_sycl.cpp`
  has exactly one `q.wait()` per scale, right before
  `vmaf_cambi_calculate_c_values`.

- **Stencil/convolution SYCL kernels MUST use `local_accessor` for tap
  reuse** (ADR-0458 / SY-2). Any kernel that implements a separable
  filter (Gaussian, box, motion-blur) with more than 3 taps **must** stage
  the required input region into shared local memory (SLM) via
  `local_accessor` + a cooperative tile-load loop + barrier, following the
  pattern in `float_vif_sycl.cpp`, `float_ansnr_sycl.cpp`,
  `float_motion_sycl.cpp`, and (post ADR-0458) `integer_ssim_sycl.cpp`.
  A bare `parallel_for<range<N>>` reading global memory for every tap is a
  lint violation for convolution kernels — use `nd_range` instead.

- **`integer_adm_sycl.cpp` / `float_adm_sycl.cpp` expose three ADM
  tuning parameters** (`adm_csf_scale`, `adm_csf_diag_scale`,
  `noise_weight`) with the same defaults as the CPU path (PR #731).
  If upstream Netflix adds or renames these parameters in
  `integer_adm.c` / `float_adm.c`, the SYCL twins must be updated
  in the same PR.

- **`motion_fps_weight` cross-backend parity** — see the canonical
  invariant note in [`../cuda/AGENTS.md`](../cuda/AGENTS.md).
  `integer_motion_v2_sycl.cpp` and `float_motion_sycl.cpp` both carry
  the `motion_fps_weight` option and apply it in `flush()` /
  `collect()` exactly as documented there. Any future change to the
  weight application math must span all motion-family GPU twins in
  the same PR.

- **VAAPI / dmabuf zero-copy import** — the FFmpeg `libvmaf_sycl`
  filter (`ffmpeg-patches/0005-*.patch`) consumes
  `vmaf_sycl_import_va_surface`. Public-surface change touches the
  patch file too — see CLAUDE.md §12 r14 +
  [ADR-0183](../../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md).

- **`ssimulacra2_sycl.cpp` IIR recurrence has no running accumulator —**
  **never 'Kahan' it** (ADR-0985). The Charalampidis recursive blur is a 3-pole
  autoregressive IIR filter
  ($o_k = n2 \cdot \text{sum} - d1 \cdot \text{prev1} - \text{prev2}$), not a
  cumulative summation. Adding an accumulator term $\text{prev1}$ into the
  output shifts the poles outside the unit circle ($1 - d1 \approx -0.8422$),
  causing geometric pole blow-up to $10^{25}$ / NaN / saturation at 100.0. The
  recurrence must remain pure float32 matching the CUDA twin
  `core/src/feature/cuda/ssimulacra2/ssimulacra2_blur.cu`. Device-level
  fp64-less divergence on Arc A380 is calibrated via
  `scripts/ci/gpu_ulp_calibration.yaml` at places=1 (`5.0e-2`), not compensated
  via pseudo-Kahan recurrence.

## icpx-aware clang-tidy

Stock LLVM `clang-tidy` cannot resolve `<sycl/sycl.hpp>`. Use
[`scripts/ci/clang-tidy-sycl.sh`](../../../../scripts/ci/clang-tidy-sycl.sh)
which injects the oneAPI SYCL include path +
`-D__SYCL_DEVICE_ONLY__=0` and locates `icpx` via `$ICPX_ROOT` (or
`/opt/intel/oneapi/compiler/latest`). The CI lane
`Tidy SYCL (advisory)` runs the wrapper.
When adding a new SYCL TU, no AGENTS.md update is needed — the
wrapper finds it via the changed-file diff. See
[ADR-0217](../../../../docs/adr/0217-sycl-toolchain-cleanup.md).

## Build

SYCL feature TUs compile only when `meson setup -Denable_sycl=true`.
Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.

## Governing ADRs

- [ADR-0182](../../../../docs/adr/0182-gpu-long-tail-batch-1.md) +
  [ADR-0188](../../../../docs/adr/0188-gpu-long-tail-batch-2.md) +
  [ADR-0192](../../../../docs/adr/0192-gpu-long-tail-batch-3.md) —
  GPU long-tail batches. Every SYCL feature kernel here corresponds
  to a row in one of these.
- [ADR-0202](../../../../docs/adr/0202-float-adm-cuda-sycl.md) +
  [ADR-0206](../../../../docs/adr/0206-ssimulacra2-cuda-sycl.md) —
  CUDA + SYCL ports that pinned `-fp-model=precise` as load-bearing.
- [ADR-0214](../../../../docs/adr/0214-gpu-parity-ci-gate.md) —
  GPU-parity CI gate.
- [ADR-0217](../../../../docs/adr/0217-sycl-toolchain-cleanup.md) —
  icpx-aware clang-tidy wrapper.
- [ADR-0219](../../../../docs/adr/0219-motion3-gpu-contract.md) —
  motion3 GPU contract.
- [ADR-0220](../../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels are unconditionally fp64-free (T7-17).
- [ADR-0243](../../../../docs/adr/0243-enable-lcs-gpu.md) — MS-SSIM
  `enable_lcs` GPU contract.
- [ADR-0985](../../../../docs/adr/0985-sycl-parity-divergence-2026-06-03.md) —
  SYCL SSIMULACRA 2 parity divergence and recurrence resolution.

## Per-kernel parity-test invariant (rounds 1–3)

Every SYCL feature kernel in this directory has a CPU twin and a
`core/test/test_sycl_<kernel>_parity.c` gate at ADR-0214 places=4
(1e-4) tolerance. The coverage matrix below tracks which SYCL kernel
maps to which CPU twin and which parity test. **On rebase**: if a
SYCL kernel is renamed or a new one is added, the parity test name +
ADR-0884 / ADR-0946 backlog must be updated in the same PR.

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

> **`speed_chroma_sycl.cpp` and `speed_temporal_sycl.cpp` are dormant
> scaffold (ADR-0957 §Context).** The source files exist (~1.5 KLOC
> combined, no TODO/FIXME markers) but are not in
> `sycl_feature_sources` in `core/src/meson.build` and the extractor
> symbols `vmaf_fex_speed_chroma_sycl` / `vmaf_fex_speed_temporal_sycl`
> are not declared/registered in `core/src/feature/feature_extractor.c`.
> Wiring them in is a separate PR — it changes the production
> extractor surface, not just test coverage. The round-4 parity
> tests are added in dormant form and auto-activate as real gates
> the day the wiring lands.

## Per-feature option-table sync invariant

**Adding a feature knob to any one backend (SYCL / CUDA / HIP / Metal /
Vulkan) requires adding it to all backends in the same PR** -- no deferred
follow-ups. The canonical source of truth for the option signature (name,
alias, type, min, max, default, flags) is the CPU feature extractor in
`core/src/feature/` (e.g. `integer_motion.c`). The GPU twins copy the
option entry verbatim and apply the weight in the equivalent host-side
`flush()` or post-processing callback.

Rationale: the CHUG / K150K extractor whitelist in
`ai/scripts/extract_k150k_features.py` passes `_feature_arg` dicts to
`vmaf_use_features_with_opts`; if the receiving backend's options table
is missing the knob the option silently falls through to the default,
producing silently-wrong scores without any error. This is the root cause
of the `motion_fps_weight` gap in `integer_motion_v2_sycl.cpp` closed by
PR #851-follow-up (2026-05-16).

Second instance (ADR-1179, `fix/sycl-v1-model-crash`): `options_cambi_sycl`
lacked `cambi_high_res_speedup` (`hrs`). Because that knob carries
`VMAF_OPT_FLAG_FEATURE_PARAM`, its absence changed the *serialised feature
name* — the SYCL twin emitted `cambi_cmxv_17_vlt_0.06` while the default
model `vmaf_v1.0.16_3d0h` asks for `cambi_hrs_1080_cmxv_17_vlt_0.06` — and
prediction failed with `-EAGAIN` instead of falling through to a default.
Two rebase-sensitive consequences: (1) every `VMAF_OPT_FLAG_FEATURE_PARAM`
knob of `cambi.c` must exist verbatim in `options_cambi_sycl`, and (2)
`vmaf_feature_name_dict_from_provided_features()` must run in
`init_fex_sycl` **before** `enc_width` / `enc_height` / `enc_bitdepth`
are defaulted from the picture geometry (same ordering as `cambi.c`),
otherwise the geometry defaults leak into the feature name. The TVI / VLT
tables come from the shared `vmaf_cambi_init_tvi_and_vlt()` in `cambi.c`
— do not reintroduce a private bisection in the twin.

## Per-kernel parity-test invariant (ADR-0214 + ADR-0868 + ADR-0884)

**Every shipping SYCL kernel here must have a CPU-vs-SYCL parity test
under [`core/test/`](../../../test/) wired into
[`core/test/meson.build`](../../../test/meson.build) with suite
`['fast', 'gpu']`.** The parity test asserts the headline score
matches the CPU scalar reference within ADR-0214 places=4 (`1e-4`)
tolerance and skips cleanly when no SYCL device is visible (mirrors
the `[skip: no SYCL device]` pattern in
[`test_sycl_motion3_parity.c`](../../../test/test_sycl_motion3_parity.c)).

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

**Rebase-sensitive**: when adding a new SYCL kernel TU, the same PR
must add the matching `test_sycl_<kernel>_parity.c` and meson
wiring. The `/cross-backend-diff` skill is a dev-time tool only and
does NOT run in CI on every PR; only the in-tree `meson test` parity
tests catch per-kernel regressions automatically.

## motion3_v2 cross-twin invariant (ADR-1108)

- `integer_motion_v2_sycl` emits `motion3_v2_score` host-side in its
  flush, mirroring the CPU `integer_motion_v2.c::flush` and the CUDA twin
  byte-for-byte: per-frame `motion_blend(motion2, blend_factor,
  blend_offset)` then `MIN(_, motion_max_val)` clip, a `stamp_value` seed
  for `i < min_idx (= 1)`, and an optional 2-tap `motion_moving_average`,
  via the shared `motion_blend_tools.h` helper. Any change to the CPU
  flush blend/clip/seed/average logic must be mirrored into all four GPU
  twins (cuda/sycl/hip/metal) in the same PR to keep the `places=4`
  `test_sycl_motion_v2_parity` gate green.
