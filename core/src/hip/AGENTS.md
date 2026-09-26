# AGENTS.md — core/src/hip

Orientation for agents working on HIP (AMD ROCm) backend. Parent:
[../../AGENTS.md](../../AGENTS.md). Mirrors
[`core/src/cuda/AGENTS.md`](../cuda/AGENTS.md) — HIP and CUDA share
near-identical async-stream + event APIs, so rebase / refactor
contracts on this side track CUDA's closely.

## Scope

HIP-side runtime (picture lifecycle, dispatch strategy, kernel
scaffolding template). HIP **feature kernels** live one level deeper
in [../feature/hip/](../feature/hip/).

```text
hip/
  common.{c,h}          # HIP context + (future) stream management
  picture_hip.{c,h}     # device picture alloc/free, and vmaf_hip_picture_upload():
                        #   the one way a host VmafPicture plane reaches the
                        #   device (waits for the copy; see ../feature/hip/AGENTS.md)
  hip_handle.h          # uintptr_t <-> hipStream_t / hipEvent_t, via a union
  dispatch_strategy.{c,h} # Feature/extractor-name → active-kernel routing
  kernel_template.{h,c} # per-feature HIP kernel scaffolding (T7-10 / ADR-0241)
  stubs.c               # -ENOSYS fallbacks for the public libvmaf_hip.h
                        #   entry points when HAVE_HIP is OFF. Wired in
                        #   from core/src/meson.build's `else` branch of
                        #   `if is_hip_enabled`, NOT via `subdir('hip')`.
                        #   Mirrors core/src/dnn/dnn_api.c's VMAF_HAVE_DNN
                        #   stub pattern.
  meson.build           # subdir() include from core/src/meson.build
```

## Backend status

Scaffold + first through eighth consumers — landed across multiple PRs.
Two additional consumers promoted from scaffold to real kernels in
ADR-0372 (batch-1, this PR).

1. **T7-10 audit-first scaffold** (ADR-0212) — common, picture, dispatch,
   feature stubs, public header `libvmaf_hip.h`, CI lane (now `Ubuntu HIP`,
   a required check), smoke-only `enable_hip` build.
   Every public C-API entry point returns `-ENOSYS`.
2. **T7-10 first consumer** (ADR-0241) — `kernel_template.{h,c}` (mirror
   of `cuda/kernel_template.h`) + `feature/hip/integer_psnr_hip.{c,h}`
   (first kernel-template consumer) + `vmaf_fex_psnr_hip` registration
   under `#if HAVE_HIP`. Template helpers and consumer's
   submit/collect return `-ENOSYS` until T7-10b.
3. **T7-10b second consumer** (ADR-0254, PR #324) —
   `feature/hip/float_psnr_hip.{c,h}` mirroring
   `feature/cuda/float_psnr_cuda.c`. Float partials precision posture.
4. **T7-10b third + fourth consumers** (ADR-0259 / ADR-0260, PR #330) —
   `ciede_hip` (`submit_pre_launch` bypass shape) and
   `float_moment_hip` (four-uint64 atomic-counter readback shape).
5. **T7-10b fifth + sixth consumers** (ADR-0266 / ADR-0267, PR #340) —
   `feature/hip/integer_motion_v2_hip.{c,h}`. (`float_ansnr_hip.{c,h}`
   was the fifth consumer per ADR-0266 but was removed in commit 70ed8b3ce3
   / PR #38; only `integer_motion_v2_hip` remains from this batch.)
   Pin (b) temporal-extractor shape with `flush()` callback +
   ping-pong buffer carry.
6. **T7-10b seventh + eighth consumers** (ADR-0273 / ADR-0274) —
   `feature/hip/float_motion_hip.{c,h}` and
   `feature/hip/float_ssim_hip.{c,h}`. Pin (a) three-buffer
   ping-pong plus `motion_force_zero` short-circuit posture, (b)
   multi-dispatch shape (`chars.n_dispatches_per_frame == 2`).
7. **T7-10b runtime landed** (2026-05-08) — `kernel_template.c` and
   `common.c` now wrap real HIP runtime calls
   (`hipStreamCreateWithFlags`, `hipEventCreateWithFlags`,
   `hipMemsetAsync`, `hipStreamWaitEvent`, `hipStreamSynchronize`,
   `hipMalloc` + `hipHostMalloc`, `hipFree` + `hipHostFree`,
   `hipGetDeviceCount`, `hipSetDevice`, `hipGetDeviceProperties`).
   `vmaf_hip_state_init` returns `0` on host with `>=1` AMD GPU;
   `-ENODEV` otherwise. `vmaf_hip_import_state` was implemented in
   ADR-0519 (2026-05-18), now lives in `core/src/libvmaf.c` next to
   CUDA / SYCL / Metal `_import_state` twins; stub body
   removed from `common.c`. Remaining feature-kernel ports follow as
   their own PRs gated by `places=4` cross-backend-diff lane
   (ADR-0214).
8. **Batch-1 real kernels** (ADR-0372) — `integer_psnr_hip` promoted
   from `-ENOSYS` scaffold to real `hipModuleLoadData` +
   `hipModuleLaunchKernel` consumer under `#ifdef HAVE_HIPCC`. Without
   `HAVE_HIPCC`, scaffold `-ENOSYS` contract preserved.
   (`float_ansnr_hip` was also promoted in ADR-0372 but subsequently
   removed in commit 70ed8b3ce3 / PR #38.)
9. **Batch-2 real kernel** (ADR-0373, this PR) — `float_motion_hip`
   promoted from `-ENOSYS` scaffold to real HIP module-API consumer.
   Adds `blur[2]` ping-pong + `ref_in` staging (`hipMalloc`) inside
   `#ifdef HAVE_HIPCC`; `compute_sad=0` on first frame; motion2 tail
   in `flush()`. Device kernel: `float_motion/float_motion_score.hip`.

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **ROCm runtime is now hard-required** when `-Denable_hip=true`.
  `meson.build` first tries `dependency('hip-lang')` via
  pkg-config + cmake, then falls back to
  `cc.find_library('amdhip64', dirs: hip_search_paths)` rooted at
  `/opt/rocm/lib` (and `HIP_PATH` if set) because ROCm 7.x ships no
  `hip-lang.pc`. Builds without `enable_hip` are unaffected.
- **HIP runtime types cross headers as `uintptr_t`**. Public header
  `libvmaf_hip.h` and kernel template's `VmafHipKernelLifecycle`
  carry `uintptr_t` for `hipStream_t` / `hipEvent_t` so consumer TUs
  stay free of `<hip/hip_runtime.h>`. Cast on implementation side
  only. Mirrors Vulkan ADR-0184 pattern.
- **Don't drift kernel-template field shape from CUDA**. Templates
  mirror each other field-for-field; runtime PR predicated on that
  mirror. See "Rebase-sensitive invariants" below.

## Rebase-sensitive invariants

- **The HIP dispatch allowlist uses exact public names.** Under
  `HAVE_HIPCC`, `g_hip_features[]` must contain each active HIP extractor's
  `.name` and every `provided_features[]` key that callers may route through
  `vmaf_hip_dispatch_supports()`. Keep the terminating `NULL` and the
  fail-closed `direct` / `none` / `disable` environment override semantics.
  Adding an extractor to `feature_extractor_list[]` without adding its exact
  dispatch names silently falls back to CPU. The repository's
  `check-dispatch-registry.sh` guards global symbol registration only; review
  the extractor's `provided_features[]` and extend the relevant HIP runtime
  test when changing this table. Commit `53c8ef155` established this coupling.

- **HIP HSACO kernel header dependency tracking**
  ([ADR-1320](../../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md);
  [Research-2106](../../../docs/research/2106-cuda-hip-kernel-header-dependency-tracking.md)):
  All HIP HSACO custom targets (`hip_hsaco_*`) in `core/src/meson.build`
  must bind `depend_files: hip_kernel_shared_headers` covering the complete
  repo-local quoted include closure, combined with compiler depfiles
  (`depfile: name + '.hsaco.d'` and
  passing `-Xclang -dependency-file -Xclang @DEPFILE@ -Xclang -MT -Xclang @OUTPUT@`
  to hipcc).
  Editing structs in shared headers (e.g. `integer_adm_cuda.h`, `vif_cuda.h`, `moment_cuda.h`)
  must reliably trigger incremental HSACO rebuilds in Ninja without manual `touch`
  workarounds. Do not remove `depend_files` or omit shared headers on rebase.

- **`kernel_template.h` mirrors `cuda/kernel_template.h`** (fork-local,
  ADR-0241). Struct shapes (`VmafHipKernelLifecycle` ↔
  `VmafCudaKernelLifecycle`, `VmafHipKernelReadback` ↔
  `VmafCudaKernelReadback`) and helper signatures
  (`vmaf_hip_kernel_lifecycle_init/_close`,
  `vmaf_hip_kernel_readback_alloc/_free`,
  `vmaf_hip_kernel_submit_pre_launch`,
  `vmaf_hip_kernel_collect_wait`) are deliberately one-to-one with
  CUDA template. Any change to CUDA template (helper signatures,
  struct fields, semantics) needs paired HIP change in same PR —
  otherwise mirror drifts, consumer call sites diverge between two
  backends. **On rebase / refactor**: if upstream port or fork PR
  touches `cuda/kernel_template.h`, walk diff onto
  `hip/kernel_template.h` + `kernel_template.c` before merging. HIP
  variant is out-of-line (`.c` paired with `.h`) instead of
  `static inline` for reason documented in `kernel_template.h`'s
  preamble — keep split until runtime PR ships, then re-evaluate. See
  [ADR-0241](../../../docs/adr/0241-hip-first-consumer-psnr.md).

- **`integer_psnr_hip.c` mirrors `integer_psnr_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0241). Same
  `PsnrStateHip`/`PsnrStateCuda` fields in same order, same
  template-helper invocations in same init/submit/collect/close
  sequence, same `provided_features` contract (`psnr_y` luma-only in
  v1). Runtime PR (T7-10b) flips `kernel_template.c` bodies;
  consumer's call sites stay verbatim. **On rebase**: keep call
  graph aligned with CUDA twin. If future PR drifts CUDA twin's
  lifecycle (e.g. adds third event), update HIP twin in same PR.

- **HIP extractor flag-promotion is per-extractor and gated on
  verified end-to-end CLI reproducer** (fork-local, ADR-0530 —
  supersedes ADR-0241 "flag bit reserved but cleared" invariant for
  `vmaf_fex_psnr_hip` and friends). Flag bit (`1 << 6`) IS now set on
  extractors that have verified-working real HIP kernel — currently
  only `vmaf_fex_integer_motion_hip` qualifies. `vmaf_fex_psnr_hip`,
  `vmaf_fex_ciede_hip`, `vmaf_fex_float_moment_hip`,
  `vmaf_fex_integer_motion_v2_hip`, `vmaf_fex_float_motion_hip`,
  `vmaf_fex_float_ssim_hip`, `vmaf_fex_float_psnr_hip`,
  `vmaf_fex_float_adm_hip`, `vmaf_fex_cambi_hip`,
  `vmaf_fex_integer_vif_hip` remain unflagged because their kernels
  are still scaffold-only / -ENOSYS / crash on first dispatch.
  `vmaf_fex_integer_vif_hip` is cautionary tale: speculatively
  flagged in its batch-1 commit, but crashes with GPU memory access
  fault on first frame when dispatch picks it; ADR-0530
  un-flags it until kernel-level fix lands. **On rebase**: do NOT
  bulk-set flag on every HIP extractor. Promotion requires its own
  ADR + `vmaf --backend hip --feature <name>` reproducer showing HIP
  kernel launching (`AMD_LOG_LEVEL=3` shows
  `hipModuleLaunchKernel` trace) and VMAF score within places=4
  cross-backend gate of CPU twin. Three companion invariants pinned
  by ADR-0530 on dispatch side:
  (1) `compute_fex_flags()` in `libvmaf.c` adds
  `VMAF_FEATURE_EXTRACTOR_HIP` whenever `vmaf->hip.state` is set
  (host-pic only, like Vulkan — no gpumask gate);
  (2) `vmaf_get_feature_extractor_by_feature_name()` falls back to
  unflagged extractor when preferred-flag pass misses, so
  partially-ported HIP backend still routes missing features
  through CPU twins;
  (3) `flush_context_serial()` drains HIP-flagged extractors'
  `gpu_pending` final-frame collect (mirrors SYCL pattern in
  `flush_context_sycl`).

## Rebase-sensitive invariants (additional consumers)

- **`ciede_hip.c` mirrors `integer_ciede_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0259). Submit path
  **intentionally does not call `vmaf_hip_kernel_submit_pre_launch`**
  because kernel writes one float per block (no atomic, no memset
  required) — CUDA twin makes same choice. **On rebase**: if future
  PR adds `submit_pre_launch` call to `integer_ciede_cuda.c`'s
  submit path, HIP twin must follow in same PR; bypass is
  load-bearing artefact this consumer pins.

- **`float_moment_hip.c` mirrors `integer_moment_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0260). Four-uint64
  atomic-counter readback (`MOMENT_HIP_COUNTERS = 4u`) sized at
  `init()` time. Submit path **does** call `submit_pre_launch`
  (kernel uses atomic adds, so memset of all four counters is
  mandatory). **On rebase**: keep four-counter constant aligned with
  CUDA twin's `4u * sizeof(uint64_t)` readback size; any drift in
  CUDA twin's counter count requires paired update here.

## Rebase-sensitive invariants (fifth + sixth consumers)

- **`float_ansnr_hip.c` (removed)**: fifth consumer per ADR-0266 was
  `float_ansnr_hip.c`, which mirrored `float_ansnr_cuda.c`. Both
  removed in commit 70ed8b3ce3 (PR #38). No-memset bypass invariant
  (`submit_pre_launch` not called; per-block `(sig, noise)` float
  partials) applied to that TU — see
  [ADR-0266](../../../docs/adr/0266-hip-fifth-consumer-float-ansnr.md)
  for historical rationale. On rebase, ignore `float_ansnr`-related
  hunks.

- **`integer_motion_v2_hip.c` mirrors `integer_motion_v2_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0267). Carries
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag and `flush()` callback.
  `uintptr_t pix[2]` ping-pong slots are fork-local scaffold-shape —
  runtime PR (T7-10b) will land HIP device-buffer allocator and
  replace these with real handles matching CUDA twin's
  `VmafCudaBuffer *pix[2]` field shape. **On rebase**: keep field
  count and slot type aligned with CUDA twin; ping-pong contract
  (cur = `index % 2`, prev = `(index + 1) % 2`) is load-bearing for
  eventual cross-backend numeric gate.

## Rebase-sensitive invariants (batch-1 real kernels — ADR-0372)

Following invariants apply to `integer_psnr_hip.c`, which was
promoted from `-ENOSYS` scaffold to real HIP Module API consumer
in ADR-0372. (`float_ansnr_hip.c` was also promoted in ADR-0372 but
was removed in commit 70ed8b3ce3 / PR #38.) These add to — and
do not replace — scaffold invariants already documented above.

- **`HAVE_HIPCC` dual-path**: all `hipModule_t` / `hipFunction_t`
  state and `psnr_hip_module_load` helpers live under
  `#ifdef HAVE_HIPCC`. Without this flag, host TU compiles without
  ROCm SDK headers and `init()` returns `-ENOSYS` (scaffold posture
  preserved). Never move device-state fields outside guard — breaks
  CPU-only CI lane.

- **`integer_psnr_hip` uint64 split-shuffle**: PSNR device kernel
  splits each uint64 warp-reduction into two uint32 `__shfl_down`
  calls (GCN/RDNA warp size = 64; HIP exposes no native uint64
  shuffle). If future ROCm release adds native uint64 shuffle
  primitives, kernel can be simplified, but cross-backend numeric
  gate must pass before landing any change:

  ```bash
  python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- \
    -C build --suite=hip-parity
  ```

- **Merge-conflict risk with PR #612**:
  `vmaf_hip_kernel_submit_post_record` in `kernel_template.{h,c}`
  and `hip_hsaco_sources` meson pipeline are also being added by PR
  #612 (`float_psnr_hip`). When two PRs merge, keep one copy,
  discard duplicate. Bodies identical so either direction safe.

## Rebase-sensitive invariants (import-state — ADR-0519)

- **`vmaf_hip_import_state` lives in `core/src/libvmaf.c`, not in
  `core/src/hip/common.c`** (fork-local, ADR-0519). Function needs
  `VmafContext` field-level access; placing it next to CUDA / SYCL /
  Metal `_import_state` twins keeps the borrowed-state implementations in
  one TU.
  Do NOT re-introduce copy of function in `hip/common.c` —
  duplicate-symbol link error is obvious failure mode, but more
  insidious one is divergent behaviour between two definitions. On
  rebase: if upstream port adds HIP-related function to
  `libvmaf.c`, leave `vmaf_hip_import_state` block intact next to
  its SYCL / Metal siblings.

- **`VmafContext::hip` substruct is appended after `metal`**
  (fork-local, ADR-0519). `hip` struct holds a single
  `VmafHipState *state` pointer gated by `#ifdef HAVE_HIP`.
  Intentionally appended at end of GPU-backend substructs so
  CPU-only / CUDA-only / etc. builds see no offset shifts. On
  rebase: if upstream reorders file-private VmafContext definition,
  keep HIP block at end. Keep its `#ifdef HAVE_HIP` guard exactly
  aligned with public-header include block at top of file.

- **HIP state lifetime mirrors SYCL / Metal, not CUDA**
  (fork-local, ADR-0519). `vmaf_close` clears `vmaf->hip.state =
  NULL` without freeing underlying state — caller owns state, frees
  it via `vmaf_hip_state_free()` only after `vmaf_close()` returns exactly 0.
  Every nonzero close retains the context and borrowed state for retry. This
  deliberately differs from CUDA twin's by-value copy semantics,
  which historically grew ownership-transfer ambiguity newer
  backends avoid. On rebase: if upstream changes CUDA twin's
  ownership model, do NOT propagate change to HIP without ADR —
  pointer-stash contract is load-bearing for caller-owned
  `VmafHipState` lifetime documented in
  `core/include/libvmaf/libvmaf_hip.h`.

## Rebase-sensitive invariants (seventh + eighth consumers)

- **`float_motion_hip.c` mirrors `float_motion_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0273). State struct
  carries three `uintptr_t` buffer slots (`ref_in`, `blur[2]`)
  tracked outside kernel-template's readback bundle; runtime PR
  (T7-10b) will swap them for real device-buffer handles matching
  CUDA twin's `VmafCudaBuffer *ref_in` + `VmafCudaBuffer *blur[2]`
  field shape. Submit path **intentionally does not call
  `vmaf_hip_kernel_submit_pre_launch`** (kernel writes per-WG SAD
  float partials directly, no atomic, no memset) — same bypass as
  `ciede_hip` and `float_ansnr_hip`. `motion_force_zero`
  short-circuit (`fex->extract` swap with
  `submit / collect / flush / close` nulled) is load-bearing and
  must stay aligned with CUDA twin. **On rebase**: any drift in
  CUDA twin's buffer-slot count or
  `motion_force_zero` posture requires paired update here.

- **`motion_fps_weight` cross-backend parity** — see canonical
  invariant note in
  [`../feature/cuda/AGENTS.md`](../feature/cuda/AGENTS.md).
  `integer_motion_v2_hip.c` and `float_motion_hip.c` both carry
  `motion_fps_weight` option and apply it identically to CUDA /
  SYCL / Metal twins. Any future change to weight
  application math must span all current motion-family GPU twins in same
  PR.

- **`float_ssim_hip.c` mirrors `integer_ssim_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0274). State struct
  carries five `uintptr_t` intermediate float buffer slots
  (`h_ref_mu`, `h_cmp_mu`, `h_ref_sq`, `h_cmp_sq`, `h_refcmp`)
  tracked outside kernel-template's readback bundle; runtime PR
  (T7-10b) will swap them for real device-buffer handles matching
  CUDA twin's `VmafCudaBuffer *h_*` field shape. Extractor reports
  `chars.n_dispatches_per_frame == 2` (first multi-dispatch HIP
  consumer); smoke test pins this value explicitly. v1 `scale=1`
  constraint surfaces as `-EINVAL` at init time before
  kernel-template's `-ENOSYS` would surface, mirroring CUDA twin's
  `compute_scale` / `vmaf_log` validation. HIP twin extracts
  `validate_dims_hip` / `init_dims_hip` helpers from `init()` to fit
  `readability-function-size` budget — CUDA twin keeps everything
  inline. **On rebase**: keep five-slot count and
  `n_dispatches_per_frame == 2` characteristic aligned with CUDA
  twin; do not re-inline helpers without verifying budget still
  passes.

## Governing ADRs

- [ADR-0212](../../../docs/adr/0212-hip-backend-scaffold.md) — HIP
  scaffold-only audit-first PR (T7-10).
- [ADR-0241](../../../docs/adr/0241-hip-first-consumer-psnr.md) —
  kernel-template mirror + `integer_psnr_hip` first consumer.
- [ADR-0254](../../../docs/adr/0254-hip-second-consumer-float-psnr.md) —
  second consumer (`float_psnr_hip`); float partials precision posture.
- [ADR-0259](../../../docs/adr/0259-hip-third-consumer-ciede.md) —
  third consumer (`ciede_hip`); pins `submit_pre_launch` bypass
  shape.
- [ADR-0260](../../../docs/adr/0260-hip-fourth-consumer-float-moment.md) —
  fourth consumer (`float_moment_hip`); pins multi-counter uint64
  readback shape.
- [ADR-0266](../../../docs/adr/0266-hip-fifth-consumer-float-ansnr.md) —
  fifth consumer (`float_ansnr_hip`); historical reference only — kernel
  and CPU twin removed in commit 70ed8b3ce3 (PR #38).
- [ADR-0267](../../../docs/adr/0267-hip-sixth-consumer-motion-v2.md) —
  sixth consumer (`motion_v2_hip`); pins temporal-extractor
  `flush()` callback + ping-pong buffer carry shape.
- [ADR-0273](../../../docs/adr/0273-hip-seventh-consumer-float-motion.md) —
  seventh consumer (`float_motion_hip`); pins three-buffer
  ping-pong (raw-pixel cache + blurred-frame ping-pong) and
  `motion_force_zero` short-circuit posture.
- [ADR-0274](../../../docs/adr/0274-hip-eighth-consumer-float-ssim.md) —
  eighth consumer (`float_ssim_hip`); pins multi-dispatch
  shape (`chars.n_dispatches_per_frame == 2`) and
  five-intermediate-float-buffer pyramid.
- [ADR-0221](../../../docs/adr/0221-gpu-kernel-template.md) — CUDA
- [ADR-0254](../../../docs/adr/0254-hip-second-consumer-float-psnr.md)
  — second consumer (`float_psnr_hip`).
- [ADR-0259](../../../docs/adr/0259-hip-third-consumer-ciede.md) —
  third consumer (`ciede_hip`).
- [ADR-0260](../../../docs/adr/0260-hip-fourth-consumer-float-moment.md)
  — fourth consumer (`float_moment_hip`).
- [ADR-0266](../../../docs/adr/0266-hip-fifth-consumer-float-ansnr.md)
  — fifth consumer (`float_ansnr_hip`); historical reference only —
  kernel removed in commit 70ed8b3ce3 (PR #38).
- [ADR-0267](../../../docs/adr/0267-hip-sixth-consumer-motion-v2.md)
  — sixth consumer (`motion_v2_hip`, this PR).
- [ADR-0372](../../../docs/adr/0372-hip-batch1-integer-psnr-float-ansnr.md) —
  batch-1 real kernels (`integer_psnr_hip`; `float_ansnr_hip` was also
  promoted here but removed in commit 70ed8b3ce3 / PR #38); pins
  `HAVE_HIPCC` dual-path and uint64 split-shuffle pattern.
- [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) — GPU
  kernel-template decision; source HIP mirror tracks.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend gate; runtime PR's incoming numerics gate.
- [ADR-0519](../../../docs/adr/0519-hip-import-state-implementation.md)
  — `vmaf_hip_import_state` implementation; moves function from
  `hip/common.c` to `libvmaf.c`, unblocks `vmaf --backend hip` on
  AMD ROCm hosts. HIP joins CUDA / SYCL / Metal as a runtime-selected
  backend (scores match CPU bit-exactly because
  dispatch still routes through CPU twins).
- [ADR-0523](../../../docs/adr/0523-hip-integer-motion-extractor-registration.md)
  — register `vmaf_fex_integer_motion_hip` (single-extractor fix).
- [ADR-0533](../../../docs/adr/0533-hip-all-extractors-registration-sweep.md)
  — full HIP-extractor registration sweep; six TUs in
  `feature/hip/` were already shipping `VmafFeatureExtractor`
  symbols but missing both from `hip_sources` and from `extern` +
  registry block in `feature_extractor.c`. Sweep pinned
  rebase-sensitive invariant: **every TU under
  `core/src/feature/hip/` that defines a `VmafFeatureExtractor
  vmaf_fex_*` symbol must appear in `hip_sources` and have matching
  `extern` + `&vmaf_fex_*` entry inside `#if HAVE_HIP` blocks of
  `core/src/feature/feature_extractor.c`**. Three legacy plumbing
  TUs (`adm_hip.c`, `vif_hip.c`, `motion_hip.c`) carry no
  `VmafFeatureExtractor`, are exempt. Two duplicate-named TUs
  (`integer_ciede_hip.c`, `integer_moment_hip.c`) are stale rename
  scaffolds whose extractor names already register via canonical
  `ciede_hip.c` / `float_moment_hip.c` TUs — leave them out of
  `hip_sources` to avoid duplicate-symbol link error.
- [ADR-1320](../../../docs/adr/1320-cuda-hip-kernel-header-dependency-tracking.md)
  — CUDA fatbin and HIP HSACO kernel header dependency tracking via explicit
  depend_files and compiler depfiles.

## Build

```bash
# CPU-only HIP build (no ROCm SDK required — scaffold -ENOSYS posture):
meson setup build -Denable_hip=true -Denable_hipcc=false \
    -Denable_cuda=false -Denable_sycl=false libvmaf
ninja -C build
python3 "$(git rev-parse --show-toplevel)/scripts/ci/run_meson_test.py" -- -C build

# Full HIP build with real kernels (requires ROCm 6+ and hipcc in PATH):
meson setup build_full -Denable_hip=true -Denable_hipcc=true \
    -Denable_cuda=false -Denable_sycl=false libvmaf
ninja -C build_full
```

The required CI lane `Ubuntu HIP` leaves `enable_hipcc` at its default
(`false`): it installs ROCm 10.0.0 from a digest-pinned image (ADR-1225) and
builds the host side without device kernels. Kernel-enabled builds
(`-Denable_hipcc=true`) require `hipcc` in `PATH` and ROCm 7.0+.

## The HIP backend is host-pic — stage before you launch (ADR-1211)

`VmafPicture::data[]` points at HOST memory for HIP (ADR-0530). HIP
kernel that reads picture planes therefore needs device copy
extractor makes itself; handing it `pic->data[i]` faults GPU:

```text
Memory access fault by GPU node-1 on address 0x... Reason: Page not present
```

and takes whole process with it — no graceful error, no skip, so
single extractor doing this makes `--backend hip` look completely
dead.

Correct shape is in `core/src/feature/hip/integer_psnr_hip.c`:
allocate per-plane device buffers in init (`hipMalloc`), copy in
extract (`hipMemcpy2DAsync`, host-to-device), free in close. Two
things to get right:

- Staged buffer is tightly packed, so stride you pass to kernel is
  plane **width**, not `pic->stride[i]`.
- Do not port CUDA twin's call shape verbatim. CUDA extractors
  receive device picture from pool, so their helpers take device
  pointer caller never had to produce. That mismatch is precisely
  how `integer_adm_hip` ended up faulting.

When debugging fault here, `AMD_SERIALIZE_KERNEL=3
HIP_LAUNCH_BLOCKING=1 AMD_LOG_LEVEL=3` names offending kernel.
Faulting address in host heap range is tell that host pointer
reached device.
