# AGENTS.md — core/src/sycl

Orientation for agents working on SYCL / DPC++ backend runtime. Parent:
[../../AGENTS.md](../../AGENTS.md).

## Scope

SYCL-side runtime (queue management, USM, dmabuf import). SYCL
**feature kernels** live in [../feature/sycl/](../feature/sycl/).

```text
sycl/
  common.cpp/.h             # queue creation, device selection, error-check
  picture_sycl.cpp/.h       # VmafPicture on a SYCL device (USM-backed)
  dmabuf_import.cpp/.h      # Linux DMA-BUF import path for zero-copy Level Zero
```

## Ground rules

- **Parent rules** apply in full: see [../../AGENTS.md](../../AGENTS.md).
- **Compiler = `icpx` (Intel oneAPI) or AdaptiveCpp `acpp` / `syclcc`
  (ADR-0335).** `clang++ -fsycl` accepted in spirit, not CI-tested. Do
  not assume MSVC-style extensions.
- **Intel-specific kernel attributes go through
  `core/src/feature/sycl/sycl_compat.h`** (ADR-0335).
  `VMAF_SYCL_REQD_SG_SIZE(N)` macro expands to
  `[[intel::reqd_sub_group_size(N)]]` under icpx, no-op under
  AdaptiveCpp. New kernel sites needing Intel-specific attribute -> add
  new macro to `sycl_compat.h`, do not hard-code attribute. **On
  rebase**: upstream cherry-pick bringing bare `[[intel::*]]` attribute
  on SYCL kernel lambda -> wrap in compat macro before merging.
- **Experimental flags enabled**: `-fsycl-unnamed-lambda`,
  `-fsycl-allow-func-ptr`, `-fsycl-device-code-split=per_kernel`. See
  [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md).
- **USM allocations**: prefer device USM for kernel-resident data,
  shared USM for cross-side communication. Host USM only when adjacent
  API requires pointer host can dereference directly.
- **dmabuf import** = Linux-only, gated at build time; no callers
  assume FD path exists on other OSes.
- **Numerical snapshots**: same rule as CUDA — see CLAUDE.md §9.
- **`-fp-model=precise` load-bearing.** SYCL feature build line in
  `core/src/meson.build` adds `-fp-model=precise` to every per-kernel
  TU. Blocks `icpx` from FMA contraction in kernel lambdas, matches
  GLSL `precise` / `NoContraction` decorations on Vulkan twins.
  Removing it drifts `float_adm_sycl`
  ([ADR-0202](../../../docs/adr/0202-float-adm-cuda-sycl.md)) past
  `places=4` at scale 2, `ssimulacra2_sycl`
  ([ADR-0206](../../../docs/adr/0206-ssimulacra2-cuda-sycl.md)) past
  `places=2` through IIR. **On rebase**: keep flag on SYCL feature
  line.
- **fp64-free kernels load-bearing
  ([ADR-0220](../../../docs/adr/0220-sycl-fp64-fallback.md), T7-17).**
  Every SYCL feature-kernel lambda must capture and operate on
  `float` / integer types only. No `double` operand inside a
  `parallel_for` body, no `sycl::reduction<double>`, no
  `sycl::plus<double>`. Hard, not soft. Single fp64 instruction
  anywhere in TU's SPIR-V module -> Level Zero runtime rejects entire
  module on Intel Arc A-series and other fp64-less devices. Applies
  even when offending kernel never submitted. `double` allowed
  *outside* kernel lambda (host-side post-processing in `extract` /
  `flush` callbacks, score aggregation, log10 normalisation). ADM gain
  limiting uses int64 Q31 (`gain_limit_to_q31` +
  `launch_decouple_csf<false>` in `integer_adm_sycl.cpp`); VIF gain
  limiting uses fp32 `sycl::fmin`. **On rebase**: upstream cherry-pick
  bringing `double` into kernel lambda -> refactor to int64 / fp32
  before merging.
- **VAAPI / dmabuf zero-copy import surface
  ([ADR-0183](../../../docs/adr/0183-ffmpeg-libvmaf-sycl-filter.md))**:
  `vmaf_sycl_import_va_surface` consumed by `libvmaf_sycl` FFmpeg
  filter (`ffmpeg-patches/0005-*.patch`). Symmetric to Vulkan VkImage
  import in
  [ADR-0186](../../../docs/adr/0186-vulkan-image-import-impl.md).
  Public surface change touches patch file too — see CLAUDE.md §12
  r14.

## Rebase-sensitive build invariants

- **`-fsycl` must be present at every link step pulling SYCL `.o`
  files from static archive.** Enforced by embedding `-fsycl` in
  `sycl_dependency.link_args` in `core/src/meson.build` (ADR-1099).
  Without `-fsycl` at link time: `icpx` skips `clang-offload-wrapper`,
  SYCL runtime's `ProgramManager` never registers device kernels,
  first `queue.submit()` crashes with null-dereference inside
  `getDeviceKernelInfo`. **On rebase**: branch splitting
  `sycl_dependency` into compile-time and link-time halves, or
  introducing new `declare_dependency` block for SYCL sub-target ->
  ensure `-fsycl` stays in link-time `link_args` of every dependency
  SYCL consumers inherit.

- **Feature-name aliasing when querying scores from non-default SYCL
  extractors.** Any `VMAF_OPT_FLAG_FEATURE_PARAM` option set to
  non-default value -> `vmaf_feature_name_from_opts_dict` stores
  scores under aliased name (not raw `VMAF_*_score` name). Aliased
  name: `<alias_of_raw_name>_<opt1_alias>_<opt2_alias>...` in
  alphabetical option order. Example: `motion_sycl` with
  `motion_add_uv=true` stores `VMAF_integer_feature_motion2_score` as
  `integer_motion2_mau`. Test code querying
  `vmaf_feature_score_at_index` must use aliased name, not raw name.
  (ADR-1099)

## Rebase-sensitive invariants per kernel

- **`feature/sycl/integer_psnr_sycl.cpp` chroma planes ride on
  per-extractor device buffers, NOT shared frame buffer.**
  `vmaf_sycl_shared_frame_init` pipeline luma-only by design (see
  `shared_*` documentation in `common.h`). Chroma upload happens in
  combined-graph `pre_fn` callback as host-staged H2D copy into
  `PsnrStateSycl::d_chroma_{ref,dis}[]`; chroma SSE kernel fires from
  `post_fn` (direct, post-graph) on same in-order combined queue. **On
  rebase**: upstream sync extending `vmaf_sycl_shared_frame_init` to
  allocate chroma planes -> PSNR extension can migrate onto it,
  per-extractor chroma buffers retire. Only after cross-backend gate
  run confirms bit-exactness against CPU at `places=4` (see
  ADR-0214). T3-15(b), ADR-0192 §"Status update 2026-05-09: T3-15 #2
  SYCL PSNR chroma".

- **`dmabuf_import.cpp` normalizes P010/P012 luma MSB→LSB on every
  import path (ADR-1121).** VA-API delivers 10/12-bit samples
  MSB-aligned (`V_MSB = V_LSB << (16 − bpc)`); VMAF feature kernels
  expect LSB-aligned values. `>> (16 − bpc)` shift (guarded
  `if (bpc > 8)`, no-op for 8-bit) applied **two ways**:
  - fused into de-tile store for Tile4 / Y-tiled paths — each sample
    shifted as written, no extra kernel, no second pass; this is the
    QSV/DG2 hot path;
  - via standalone `launch_p010_normalize()` kernel for rare LINEAR
    D2D and readback paths (no per-sample store to fuse into), whose
    event threads into `vmaf_sycl_set_detile_event()` (or gets
    subsumed by `q->wait_and_throw()` on readback).

  **On rebase**: shift must stay on *every* import path — dropping it
  on any one re-introduces the 64× `integer_motion` / NaN bug for that
  tiling mode; and do **not** re-add a standalone full-plane normalize
  pass on tiled paths (cost ~15% throughput at 4K — keep it fused).
  Chroma needs no normalization (shared frame pipeline luma-only,
  above). **Do not add a `DMA_BUF_IOCTL_SYNC` coherency flush** — tried
  and removed: insufficient for the contamination (real fix = separate
  -per-decoder-QSV-session contract, FIX-03), and its `SYNC_START`
  blocking fence-wait serialised decode→compute.

- **Zero-copy VA-import path defaults to DIRECT dispatch, not
  combined graph (ADR-1121).** `vmaf_sycl_select_strategy()` takes
  `va_import_path` flag (passed `state->has_imported` from
  `common.cpp`), returns DIRECT for it. Applied *after* env-override
  checks -> `VMAF_SYCL_USE_GRAPH=1` / `VMAF_SYCL_DISPATCH=…:graph`
  still force graph. Graph = net throughput loss on this path
  (byte-identical output, but per-frame de-tile import + graph
  compute-barrier serialise decode→compute, ~15–25% slower at 4K).
  **On rebase**: keep `va_import_path` threaded from `has_imported`;
  do not let area-threshold heuristic (tuned for host-upload) re-select
  graph for VA-import.

- **`common.cpp` cleanup + helper boundaries (HISS-21 burn-down).**
  `sycl_shared_frame_release()` is the single cleanup owner for the shared
  frame buffers; it replaced the `fail:` label that
  `vmaf_sycl_shared_frame_init` used before the HISS-01 burn-down. Its body is
  that label block verbatim — the loop frees `shared_ref_buf[i]` before
  `shared_dis_buf[i]` for `i = 0` then `i = 1`, each guarded by its own null
  check, then nulls all four slots — so every error exit frees the same
  buffers in the same order, and partially-allocated states still rely on
  those null checks. The helper is idempotent. The `static` helpers extracted
  alongside it (`sycl_resolve_device`, `sycl_log_fp64_note`,
  `sycl_profiling_enabled`, `sycl_queue_props`, `sycl_enqueue_plane_upload`,
  `sycl_any_extractor_wants_graph`, `sycl_run_compute_phase`,
  `sycl_apply_input_barriers`, `sycl_enqueue_all_phases`) exist to hold their
  callers inside the HISS-04 60-LOC bound. **On rebase**: do not reintroduce
  `goto fail` or an early return between an allocation and the release call;
  do not move these helpers to another TU or behind a function pointer (that
  changes inlining and was not what the burn-down verified); keep
  `sycl_resolve_device` and `sycl_enqueue_all_phases` called from *inside*
  their caller's existing `try` so a `sycl::exception` never crosses the
  `extern "C"` frame; preserve the in-order enqueue order (ref before dis in
  `vmaf_sycl_shared_frame_upload`; graph recording, then `pre_fn`, then
  compute, then `post_fn` in `sycl_enqueue_all_phases`); and keep
  `sycl_enqueue_plane_upload`'s original `static_cast<unsigned>` narrowing on
  the stride comparison — widening it to `size_t` changes which pictures take
  the bulk-memcpy path.

- **`vmaf_sycl_graph_register` allocation order (ADR-0982 / BUG-048 Sec A3).**
  In `vmaf_sycl_graph_register()` (`common.cpp`), ensure `state->combined_queue`
  is allocated (via `std::make_unique<sycl::queue>`) *before* incrementing
  `state->num_graph_extractors` or populating the extractor entry. If queue
  creation throws or fails, `num_graph_extractors` must not be left dirty with
  an uninitialized entry in `graph_extractors[]`. On rebase: preserve this
  allocation-before-registration sequence.

- [ADR-0002](../../../docs/adr/0002-merge-path-master-default.md) —
  sycl branch → master merge history.
- [ADR-0016](../../../docs/adr/0016-sycl-to-master-merge-conflict-policy.md)
  — merge-conflict policy.
- [ADR-0022](../../../docs/adr/0022-inference-runtime-onnx.md) —
  OpenVINO EP mapping for SYCL.
- [ADR-0027](../../../docs/adr/0027-non-conservative-image-pins.md) —
  experimental SYCL flags.
- [ADR-0220](../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels are unconditionally fp64-free (T7-17).
- [ADR-0335](../../../docs/adr/0335-adaptivecpp-second-sycl-toolchain.md)
  — AdaptiveCpp added as a second SYCL toolchain alongside icpx;
  Intel-specific kernel attributes routed through
  `feature/sycl/sycl_compat.h`.
- [ADR-0982](../../../docs/adr/0982-gpu-runtime-bug-audit-round-26.md) —
  GPU runtime bug audit — round 26 (init/teardown leak sweep).
- [ADR-1121](../../../docs/adr/1121-sycl-qsv-zerocopy-p010-normalization.md)
  — QSV zero-copy P010/P012 MSB→LSB normalization in the VA import +
  separate-per-decoder-session decode contract.

## Build

```bash
meson setup build -Denable_cuda=false -Denable_sycl=true
ninja -C build
```

Requires oneAPI (`source /opt/intel/oneapi/setvars.sh`) or equivalent
DPC++ toolchain with `icpx` on PATH.

Alternative: AdaptiveCpp (open-source, ADR-0335).

```bash
meson setup build-acpp -Denable_cuda=false -Denable_sycl=true \
    -Dsycl_compiler=acpp -Dsycl_acpp_targets=generic
ninja -C build-acpp
```

See
[`docs/development/sycl-toolchains.md`](../../../docs/development/sycl-toolchains.md)
for per-toolchain capability matrix and numerical conformance notes.
