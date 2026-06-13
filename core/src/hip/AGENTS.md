# AGENTS.md — core/src/hip

Orientation for agents working on the HIP (AMD ROCm) backend. Parent:
[../../AGENTS.md](../../AGENTS.md). Mirrors
[`core/src/cuda/AGENTS.md`](../cuda/AGENTS.md) — HIP and CUDA share
near-identical async-stream + event APIs, so the rebase / refactor
contracts on this side track CUDA's closely.

## Scope

The HIP-side runtime (picture lifecycle, dispatch strategy, kernel
scaffolding template). HIP **feature kernels** live one level deeper
in [../feature/hip/](../feature/hip/).

```text
hip/
  common.{c,h}          # HIP context + (future) stream management
  picture_hip.{c,h}     # VmafPicture on a HIP device — stub
  dispatch_strategy.{c,h} # Feature-name → kernel routing — stub
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
   feature stubs, public header `libvmaf_hip.h`, CI lane
   `Build — Ubuntu HIP (T7-10 scaffold)`, smoke-only `enable_hip` build.
   Every public C-API entry point returns `-ENOSYS`.
2. **T7-10 first consumer** (ADR-0241) — `kernel_template.{h,c}` (mirror
   of `cuda/kernel_template.h`) + `feature/hip/integer_psnr_hip.{c,h}`
   (first kernel-template consumer) + `vmaf_fex_psnr_hip` registration
   under `#if HAVE_HIP`. Template helpers and the consumer's
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
   Pin (b) the temporal-extractor shape with `flush()` callback +
   ping-pong buffer carry.
6. **T7-10b seventh + eighth consumers** (ADR-0273 / ADR-0274) —
   `feature/hip/float_motion_hip.{c,h}` and
   `feature/hip/float_ssim_hip.{c,h}`. Pin (a) the three-buffer
   ping-pong plus the `motion_force_zero` short-circuit posture, (b)
   the multi-dispatch shape (`chars.n_dispatches_per_frame == 2`).
7. **T7-10b runtime landed** (2026-05-08) — `kernel_template.c` and
   `common.c` now wrap real HIP runtime calls
   (`hipStreamCreateWithFlags`, `hipEventCreateWithFlags`,
   `hipMemsetAsync`, `hipStreamWaitEvent`, `hipStreamSynchronize`,
   `hipMalloc` + `hipHostMalloc`, `hipFree` + `hipHostFree`,
   `hipGetDeviceCount`, `hipSetDevice`, `hipGetDeviceProperties`).
   `vmaf_hip_state_init` returns `0` on a host with `>=1` AMD GPU;
   `-ENODEV` otherwise. `vmaf_hip_import_state` was implemented in
   ADR-0519 (2026-05-18) and now lives in `core/src/libvmaf.c`
   next to the CUDA / SYCL / Vulkan / Metal `_import_state` twins;
   the stub body has been removed from `common.c`. Remaining
   feature-kernel ports follow as their own PRs gated by the
   `places=4` cross-backend-diff lane (ADR-0214).
8. **Batch-1 real kernels** (ADR-0372) — `integer_psnr_hip` promoted
   from `-ENOSYS` scaffold to a real `hipModuleLoadData` +
   `hipModuleLaunchKernel` consumer under `#ifdef HAVE_HIPCC`. Without
   `HAVE_HIPCC`, the scaffold `-ENOSYS` contract is preserved.
   (`float_ansnr_hip` was also promoted in ADR-0372 but subsequently
   removed in commit 70ed8b3ce3 / PR #38.)
9. **Batch-2 real kernel** (ADR-0373, this PR) — `float_motion_hip`
   promoted from `-ENOSYS` scaffold to a real HIP module-API consumer.
   Adds `blur[2]` ping-pong + `ref_in` staging (`hipMalloc`) inside
   `#ifdef HAVE_HIPCC`; `compute_sad=0` on first frame; motion2 tail
   in `flush()`. Device kernel: `float_motion/float_motion_score.hip`.

## Ground rules

- **Parent rules** apply in full (see [../../AGENTS.md](../../AGENTS.md)).
- **ROCm runtime is now hard-required** when `-Denable_hip=true`.
  The `meson.build` first tries `dependency('hip-lang')` via
  pkg-config + cmake, then falls back to
  `cc.find_library('amdhip64', dirs: hip_search_paths)` rooted at
  `/opt/rocm/lib` (and `HIP_PATH` if set) because ROCm 7.x ships
  no `hip-lang.pc`. Builds without `enable_hip` are unaffected.
- **HIP runtime types cross headers as `uintptr_t`**. The public
  header `libvmaf_hip.h` and the kernel template's `VmafHipKernelLifecycle`
  carry `uintptr_t` for `hipStream_t` / `hipEvent_t` so consumer TUs
  stay free of `<hip/hip_runtime.h>`. Cast on the implementation side
  only. Mirrors the Vulkan ADR-0184 pattern.
- **Don't drift the kernel-template field shape from CUDA**. The
  templates mirror each other field-for-field; the runtime PR is
  predicated on that mirror. See "Rebase-sensitive invariants" below.

## Rebase-sensitive invariants

- **`kernel_template.h` mirrors `cuda/kernel_template.h`** (fork-local,
  ADR-0241). The struct shapes (`VmafHipKernelLifecycle` ↔
  `VmafCudaKernelLifecycle`, `VmafHipKernelReadback` ↔
  `VmafCudaKernelReadback`) and the helper signatures
  (`vmaf_hip_kernel_lifecycle_init/_close`,
  `vmaf_hip_kernel_readback_alloc/_free`,
  `vmaf_hip_kernel_submit_pre_launch`,
  `vmaf_hip_kernel_collect_wait`) are deliberately one-to-one with
  the CUDA template. Any change to the CUDA template (helper
  signatures, struct fields, semantics) needs a paired HIP change
  in the same PR — otherwise the mirror drifts and consumer call
  sites diverge between the two backends. **On rebase / refactor**:
  if an upstream port or a fork PR touches `cuda/kernel_template.h`,
  walk the diff onto `hip/kernel_template.h` + `kernel_template.c`
  before merging. The HIP variant is out-of-line (`.c` paired with
  `.h`) instead of `static inline` for the reason documented in
  `kernel_template.h`'s preamble — keep the split until the runtime
  PR ships, then re-evaluate. See
  [ADR-0241](../../../docs/adr/0241-hip-first-consumer-psnr.md).

- **`integer_psnr_hip.c` mirrors `integer_psnr_cuda.c` call-graph-for-call-graph**
  (fork-local, ADR-0241). Same `PsnrStateHip`/`PsnrStateCuda` fields
  in the same order, same template-helper invocations in the same
  init/submit/collect/close sequence, same `provided_features`
  contract (`psnr_y` luma-only in v1). The runtime PR (T7-10b) flips
  the `kernel_template.c` bodies; the consumer's call sites stay
  verbatim. **On rebase**: keep the call graph aligned with the
  CUDA twin. If a future PR drifts the CUDA twin's lifecycle (e.g.
  adds a third event), update the HIP twin in the same PR.

- **HIP extractor flag-promotion is per-extractor and gated on a
  verified end-to-end CLI reproducer** (fork-local, ADR-0530 —
  supersedes the ADR-0241 "flag bit reserved but cleared" invariant
  for `vmaf_fex_psnr_hip` and friends). The flag bit (`1 << 6`)
  IS now set on extractors that have a verified-working real HIP
  kernel — currently only `vmaf_fex_integer_motion_hip` qualifies.
  `vmaf_fex_psnr_hip`, `vmaf_fex_ciede_hip`,
  `vmaf_fex_float_moment_hip`,
  `vmaf_fex_integer_motion_v2_hip`, `vmaf_fex_float_motion_hip`,
  `vmaf_fex_float_ssim_hip`, `vmaf_fex_float_psnr_hip`,
  `vmaf_fex_float_adm_hip`, `vmaf_fex_cambi_hip`,
  `vmaf_fex_integer_vif_hip` remain unflagged because their kernels
  are still scaffold-only / -ENOSYS / crash on first dispatch.
  `vmaf_fex_integer_vif_hip` is the cautionary tale: it was
  speculatively flagged in its batch-1 commit but crashes with a
  GPU memory access fault on the first frame when the dispatch
  actually picks it; ADR-0530 un-flags it until the kernel-level
  fix lands. **On rebase**: do NOT bulk-set the flag on every HIP
  extractor — promotion requires its own ADR + a
  `vmaf --backend hip --feature <name>` reproducer that shows the
  HIP kernel actually launching (`AMD_LOG_LEVEL=3` shows the
  `hipModuleLaunchKernel` trace) and that the VMAF score is within
  the places=4 cross-backend gate of the CPU twin. Three companion
  invariants pinned by ADR-0530 on the dispatch side:
  (1) `compute_fex_flags()` in `libvmaf.c` adds
  `VMAF_FEATURE_EXTRACTOR_HIP` whenever `vmaf->hip.state` is set
  (host-pic only, like Vulkan — no gpumask gate);
  (2) `vmaf_get_feature_extractor_by_feature_name()` falls back to
  an unflagged extractor when the preferred-flag pass misses, so a
  partially-ported HIP backend still routes the missing features
  through CPU twins;
  (3) `flush_context_serial()` drains HIP-flagged extractors'
  `gpu_pending` final-frame collect (mirrors the SYCL pattern in
  `flush_context_sycl`).

## Rebase-sensitive invariants (additional consumers)

- **`ciede_hip.c` mirrors `integer_ciede_cuda.c` call-graph-for-call-graph**
  (fork-local, ADR-0259). The submit path **intentionally does not
  call `vmaf_hip_kernel_submit_pre_launch`** because the kernel
  writes one float per block (no atomic, no memset required) — the
  CUDA twin makes the same choice. **On rebase**: if a future PR
  adds a `submit_pre_launch` call to `integer_ciede_cuda.c`'s
  submit path, the HIP twin must follow in the same PR; the bypass
  is the load-bearing artefact this consumer pins.

- **`float_moment_hip.c` mirrors `integer_moment_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0260). Four-uint64
  atomic-counter readback (`MOMENT_HIP_COUNTERS = 4u`) sized at
  `init()` time. The submit path **does** call
  `submit_pre_launch` (the kernel uses atomic adds, so the memset
  of all four counters is mandatory). **On rebase**: keep the
  four-counter constant aligned with the CUDA twin's
  `4u * sizeof(uint64_t)` readback size; any drift in the CUDA
  twin's counter count requires a paired update here.

## Rebase-sensitive invariants (fifth + sixth consumers)

- **`float_ansnr_hip.c` (removed)**: the fifth consumer per ADR-0266 was
  `float_ansnr_hip.c`, which mirrored `float_ansnr_cuda.c`. Both were
  removed in commit 70ed8b3ce3 (PR #38). The no-memset bypass invariant
  (`submit_pre_launch` not called; per-block `(sig, noise)` float partials)
  applied to that TU — see [ADR-0266](../../../docs/adr/0266-hip-fifth-consumer-float-ansnr.md)
  for the historical rationale. On rebase, ignore `float_ansnr`-related hunks.

- **`integer_motion_v2_hip.c` mirrors `integer_motion_v2_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0267). Carries the
  `VMAF_FEATURE_EXTRACTOR_TEMPORAL` flag and the `flush()` callback.
  The `uintptr_t pix[2]` ping-pong slots are a fork-local
  scaffold-shape — the runtime PR (T7-10b) will land a HIP
  device-buffer allocator and replace these with real handles
  matching the CUDA twin's `VmafCudaBuffer *pix[2]` field shape.
  **On rebase**: keep the field count and slot type aligned with
  the CUDA twin; the ping-pong contract (cur = `index % 2`, prev =
  `(index + 1) % 2`) is load-bearing for the eventual cross-backend
  numeric gate.

## Rebase-sensitive invariants (batch-1 real kernels — ADR-0372)

The following invariants apply to `integer_psnr_hip.c`, which was
promoted from a `-ENOSYS` scaffold to a real HIP Module API consumer
in ADR-0372. (`float_ansnr_hip.c` was also promoted in ADR-0372 but
was removed in commit 70ed8b3ce3 / PR #38.) These add to — and
do not replace — the scaffold invariants already documented above.

- **`HAVE_HIPCC` dual-path**: all `hipModule_t` / `hipFunction_t`
  state and the `psnr_hip_module_load` helpers live under
  `#ifdef HAVE_HIPCC`. Without this flag the host TU compiles without
  ROCm SDK headers and `init()` returns `-ENOSYS` (scaffold posture
  preserved). Never move device-state fields outside the guard — it
  breaks the CPU-only CI lane.

- **`integer_psnr_hip` uint64 split-shuffle**: the PSNR device kernel
  splits each uint64 warp-reduction into two uint32 `__shfl_down`
  calls (GCN/RDNA warp size = 64; HIP exposes no native uint64
  shuffle). If a future ROCm release adds native uint64 shuffle
  primitives, the kernel can be simplified, but the cross-backend
  numeric gate (`meson test -C build --suite=hip-parity`) must pass
  before landing any change.

- **Merge-conflict risk with PR #612**: `vmaf_hip_kernel_submit_post_record`
  in `kernel_template.{h,c}` and the `hip_hsaco_sources` meson pipeline
  are also being added by PR #612 (`float_psnr_hip`). When the two PRs
  merge, keep one copy and discard the duplicate. The bodies are
  identical so either direction is safe.

## Rebase-sensitive invariants (import-state — ADR-0519)

- **`vmaf_hip_import_state` lives in `core/src/libvmaf.c`, not in
  `core/src/hip/common.c`** (fork-local, ADR-0519). The function
  needs `VmafContext` field-level access; placing it next to the
  CUDA / SYCL / Vulkan / Metal `_import_state` twins keeps the four
  "stash the borrowed state pointer on the context" implementations
  in one TU. Do NOT re-introduce a copy of the function in
  `hip/common.c` — the duplicate-symbol link error is the obvious
  failure mode, but the more insidious one is divergent behaviour
  between the two definitions. On rebase: if any upstream port adds
  a HIP-related function to `libvmaf.c`, leave the `vmaf_hip_import_state`
  block intact next to its SYCL / Vulkan / Metal siblings.

- **`VmafContext::hip` substruct is appended after `metal`**
  (fork-local, ADR-0519). The `hip` struct holds a single
  `VmafHipState *state` pointer gated by `#ifdef HAVE_HIP`. It is
  intentionally appended at the end of the GPU-backend substructs
  so CPU-only / CUDA-only / etc. builds see no offset shifts. On
  rebase: if upstream reorders the file-private VmafContext
  definition, keep the HIP block at the end and keep its
  `#ifdef HAVE_HIP` guard exactly aligned with the public-header
  include block at the top of the file.

- **HIP state lifetime mirrors SYCL / Vulkan / Metal, not CUDA**
  (fork-local, ADR-0519). `vmaf_close` clears `vmaf->hip.state =
  NULL` without freeing the underlying state — caller owns the
  state and frees it via `vmaf_hip_state_free()` after `vmaf_close`.
  This deliberately differs from the CUDA twin's by-value copy
  semantics, which historically grew an ownership-transfer
  ambiguity the newer backends avoid. On rebase: if upstream
  changes the CUDA twin's ownership model, do NOT propagate the
  change to HIP without an ADR — the pointer-stash contract is
  load-bearing for the caller-owned `VmafHipState` lifetime
  documented in `core/include/libvmaf/libvmaf_hip.h`.

## Rebase-sensitive invariants (seventh + eighth consumers)

- **`float_motion_hip.c` mirrors `float_motion_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0273). The state
  struct carries three `uintptr_t` buffer slots (`ref_in`,
  `blur[2]`) tracked outside the kernel-template's readback bundle;
  the runtime PR (T7-10b) will swap them for real device-buffer
  handles matching the CUDA twin's `VmafCudaBuffer *ref_in` +
  `VmafCudaBuffer *blur[2]` field shape. The submit path
  **intentionally does not call `vmaf_hip_kernel_submit_pre_launch`**
  (kernel writes per-WG SAD float partials directly, no atomic,
  no memset) — same bypass as `ciede_hip` and `float_ansnr_hip`.
  The `motion_force_zero` short-circuit (`fex->extract` swap with
  `submit / collect / flush / close` nulled) is load-bearing and
  must stay aligned with the CUDA twin. **On rebase**: any drift
  in the CUDA twin's buffer-slot count or the `motion_force_zero`
  posture requires a paired update here.

- **`motion_fps_weight` cross-backend parity** — see the canonical
  invariant note in
  [`../feature/cuda/AGENTS.md`](../feature/cuda/AGENTS.md).
  `integer_motion_v2_hip.c` and `float_motion_hip.c` both carry
  the `motion_fps_weight` option and apply it identically to the
  CUDA / SYCL / Vulkan / Metal twins. Any future change to the weight
  application math must span all motion-family GPU twins in the same PR.

- **`float_ssim_hip.c` mirrors `integer_ssim_cuda.c`
  call-graph-for-call-graph** (fork-local, ADR-0274). The state
  struct carries five `uintptr_t` intermediate float buffer slots
  (`h_ref_mu`, `h_cmp_mu`, `h_ref_sq`, `h_cmp_sq`, `h_refcmp`)
  tracked outside the kernel-template's readback bundle; the
  runtime PR (T7-10b) will swap them for real device-buffer
  handles matching the CUDA twin's `VmafCudaBuffer *h_*` field
  shape. The extractor reports `chars.n_dispatches_per_frame == 2`
  (first multi-dispatch HIP consumer); the smoke test pins this
  value explicitly. The v1 `scale=1` constraint surfaces as
  `-EINVAL` at init time before the kernel-template's `-ENOSYS`
  would surface, mirroring the CUDA twin's `compute_scale` /
  `vmaf_log` validation. The HIP twin extracts `validate_dims_hip`
  / `init_dims_hip` helpers from `init()` to fit the
  `readability-function-size` budget — the CUDA twin keeps
  everything inline. **On rebase**: keep the five-slot count and
  the `n_dispatches_per_frame == 2` characteristic aligned with
  the CUDA twin; do not re-inline the helpers without verifying
  the budget still passes.

## Governing ADRs

- [ADR-0212](../../../docs/adr/0212-hip-backend-scaffold.md) — HIP
  scaffold-only audit-first PR (T7-10).
- [ADR-0241](../../../docs/adr/0241-hip-first-consumer-psnr.md) —
  kernel-template mirror + `integer_psnr_hip` first consumer.
- [ADR-0254](../../../docs/adr/0254-hip-second-consumer-float-psnr.md) —
  second consumer (`float_psnr_hip`); float partials precision posture.
- [ADR-0259](../../../docs/adr/0259-hip-third-consumer-ciede.md) —
  third consumer (`ciede_hip`); pins the `submit_pre_launch` bypass
  shape.
- [ADR-0260](../../../docs/adr/0260-hip-fourth-consumer-float-moment.md) —
  fourth consumer (`float_moment_hip`); pins the multi-counter
  uint64 readback shape.
- [ADR-0266](../../../docs/adr/0266-hip-fifth-consumer-float-ansnr.md) —
  fifth consumer (`float_ansnr_hip`); historical reference only — kernel
  and CPU twin removed in commit 70ed8b3ce3 (PR #38).
- [ADR-0267](../../../docs/adr/0267-hip-sixth-consumer-motion-v2.md) —
  sixth consumer (`motion_v2_hip`); pins the temporal-extractor
  `flush()` callback + ping-pong buffer carry shape.
- [ADR-0273](../../../docs/adr/0273-hip-seventh-consumer-float-motion.md) —
  seventh consumer (`float_motion_hip`); pins the three-buffer
  ping-pong (raw-pixel cache + blurred-frame ping-pong) and the
  `motion_force_zero` short-circuit posture.
- [ADR-0274](../../../docs/adr/0274-hip-eighth-consumer-float-ssim.md) —
  eighth consumer (`float_ssim_hip`); pins the multi-dispatch
  shape (`chars.n_dispatches_per_frame == 2`) and the
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
  promoted here but removed in commit 70ed8b3ce3 / PR #38); pins the
  `HAVE_HIPCC` dual-path and the uint64 split-shuffle pattern.
- [ADR-0246](../../../docs/adr/0246-gpu-kernel-template.md) — GPU
  kernel-template decision; the source the HIP mirror tracks.
- [ADR-0214](../../../docs/adr/0214-gpu-parity-ci-gate.md) — `places=4`
  cross-backend gate; the runtime PR's incoming numerics gate.
- [ADR-0519](../../../docs/adr/0519-hip-import-state-implementation.md)
  — `vmaf_hip_import_state` implementation; moves the function from
  `hip/common.c` to `libvmaf.c`, unblocks `vmaf --backend hip` on
  AMD ROCm hosts. HIP joins CUDA / SYCL / Vulkan as a fully working
  runtime-selected backend (scores match CPU bit-exactly because
  dispatch still routes through CPU twins).
- [ADR-0523](../../../docs/adr/0523-hip-integer-motion-extractor-registration.md)
  — register `vmaf_fex_integer_motion_hip` (single-extractor fix).
- [ADR-0533](../../../docs/adr/0533-hip-all-extractors-registration-sweep.md)
  — full HIP-extractor registration sweep; six TUs in
  `feature/hip/` were already shipping `VmafFeatureExtractor`
  symbols but missing both from `hip_sources` and from the
  `extern` + registry block in `feature_extractor.c`. The sweep
  pinned the rebase-sensitive invariant: **every TU under
  `core/src/feature/hip/` that defines a `VmafFeatureExtractor
  vmaf_fex_*` symbol must appear in `hip_sources` and have a
  matching `extern` + `&vmaf_fex_*` entry inside the `#if HAVE_HIP`
  blocks of `core/src/feature/feature_extractor.c`**. The three
  legacy plumbing TUs (`adm_hip.c`, `vif_hip.c`, `motion_hip.c`)
  carry no `VmafFeatureExtractor` and are exempt. The two
  duplicate-named TUs (`integer_ciede_hip.c`,
  `integer_moment_hip.c`) are stale rename scaffolds whose
  extractor names already register via the canonical
  `ciede_hip.c` / `float_moment_hip.c` TUs — leave them out of
  `hip_sources` to avoid a duplicate-symbol link error.

## Build

```bash
# CPU-only HIP build (no ROCm SDK required — scaffold -ENOSYS posture):
meson setup build -Denable_hip=true -Denable_hipcc=false \
    -Denable_cuda=false -Denable_sycl=false libvmaf
ninja -C build
meson test -C build

# Full HIP build with real kernels (requires ROCm 6+ and hipcc in PATH):
meson setup build_full -Denable_hip=true -Denable_hipcc=true \
    -Denable_cuda=false -Denable_sycl=false libvmaf
ninja -C build_full
```

The CI lane `Build — Ubuntu HIP (T7-10 scaffold)` uses
`-Denable_hipcc=false` so it runs without a ROCm SDK. Kernel-enabled
builds (`-Denable_hipcc=true`) require `hipcc` in `PATH` and ROCm 6+.
