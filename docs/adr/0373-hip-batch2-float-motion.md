<!-- markdownlint-disable MD013 MD060 -->
# ADR-0373: HIP Batch-2 — `float_motion_hip` Real Kernel

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `build`

## Context

ADR-0372 promoted `integer_psnr_hip` and `float_ansnr_hip` from `-ENOSYS`
stubs to real HIP module-API consumers (batch-1). It explicitly deferred
`float_motion_hip` because the existing scaffold used a different API shape
(opaque `uintptr_t` slots instead of real `hipMalloc` device buffers) and the
temporal design (blur ping-pong + `flush()` tail emission) required more
adaptation than the stateless scalar-reduction targets.

After reviewing the scaffold, the adaptation is mechanical: the HIP device
kernel already existed at `feature/hip/float_motion/float_motion_score.hip`
(a warp-64 port of the CUDA twin), and the host-side changes follow the same
`hipModuleLoadData` + `hipModuleGetFunction` + `hipModuleLaunchKernel` pattern
established by ADR-0254 and ADR-0372. The only non-trivial aspect is the
temporal state: `blur[2]` ping-pong (two `hipMalloc` float arrays) +
`ref_in` staging buffer, with `compute_sad=0` on the first frame and the
`prev_motion_score` carry for motion2.

## Decision

Promote `float_motion_hip` (emits `VMAF_feature_motion_score` and
`VMAF_feature_motion2_score`) from `-ENOSYS` scaffold to a real HIP
module-API consumer following ADR-0254 / ADR-0372.

Key implementation choices:

1. **Struct layout**: Replace the three `uintptr_t` opaque slots (`ref_in`,
   `blur[0]`, `blur[1]`) with real `void *` device pointers allocated via
   `hipMalloc`. Module and per-bpc function handles (`hipModule_t module`,
   `hipFunction_t funcbpc8/funcbpc16`) added inside `#ifdef HAVE_HIPCC`.

2. **Helper extraction**: `fm_hip_bufs_alloc` and `fm_hip_bufs_free` are
   extracted to keep `init_fex_hip` under the 60-line function-size limit.
   `fm_hip_bufs_free` also unloads the module (single point of teardown).

3. **Temporal logic**: `compute_sad=0` on the first frame (no previous blur
   available; kernel writes `cur_blur` but partials are all 0.0 by contract).
   `collect()` emits `motion2_score = min(prev, cur)` at `index - 1` and
   `flush()` emits the tail `motion2_score` at `s->index`. Mirrors the CUDA
   twin's `flush_fex_cuda` exactly.

4. **HtoD copy**: `hipMemcpy2DAsync` with `hipMemcpyHostToDevice` because
   pictures arrive as CPU `VmafPicture`s (`VMAF_FEATURE_EXTRACTOR_HIP` flag
   not yet set — same posture as all prior HIP consumers).

5. **Meson**: `float_motion_score` added to `hip_kernel_sources` in
   `src/meson.build` so `hipcc --genco` + `xxd -i` embed the HSACO blob at
   build time when `enable_hipcc=true`.

6. **Non-HAVE_HIPCC path**: scaffold posture preserved; `init()` returns
   `-ENOSYS` via the template helpers, `submit()` calls
   `vmaf_hip_kernel_submit_pre_launch` and returns `-ENOSYS`, `collect()`
   and `flush()` return `-ENOSYS`/`1` respectively. CPU-only CI is unaffected.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port `float_moment_hip` as batch-2 instead | Stateless, simpler | Kernel already implemented; `float_moment_hip.c` needs its own `HAVE_HIPCC` promotion separately | Both can land; `float_motion` is higher priority (used by VMAF models directly) |
| Port `float_ssim_hip` | Completes more extractors | Two-pass design, five intermediate device buffers, three kernel functions; non-trivial ABI | Per per-task guidance: STOP if bit-exactness is not trivially clear |
| Keep `motion_hip` (the old raw-API stub) | No change | It uses a different API shape and is not a `VmafFeatureExtractor` consumer | It is a separate implementation; `float_motion_hip` is the correct consumer |

## Consequences

- **Positive**: `float_motion_hip` is now a real kernel consumer. HIP extractor
  count rises from 3/11 real (ADR-0372) to 4/11 real. `VMAF_feature_motion_score`
  and `VMAF_feature_motion2_score` are now GPU-accelerated on ROCm.
- **Neutral**: `float_ssim_hip`, `float_moment_hip`, `integer_motion_v2_hip`,
  `adm_hip`, `vif_hip`, `ciede_hip` remain at `-ENOSYS` scaffold posture.
- **Follow-up**: `float_moment_hip` and `integer_motion_v2_hip` are the next
  simplest batch-3 candidates (the device kernels already exist).

## References

- ADR-0273 (`float_motion_hip` seventh consumer scaffold).
- ADR-0372 (HIP batch-1: `integer_psnr_hip` + `float_ansnr_hip`).
- ADR-0254 (`float_psnr_hip` canonical HIP module-API pattern).
- paraphrased: the user requested recovery of the previous batch-2 agent's work on `float_motion_hip` — promoting the host TU from `-ENOSYS` scaffold to a real HIP module-API consumer using the kernel already present in `float_motion/float_motion_score.hip`.
