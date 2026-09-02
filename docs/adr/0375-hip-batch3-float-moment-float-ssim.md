<!-- markdownlint-disable MD060 -->
# ADR-0375: HIP batch-3 — `float_moment_hip` and `float_ssim_hip` real kernels

- **Status**: Accepted
- **Date**: 2026-05-10
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `build`, `feature-extractor`, `fork-local`

## Context

After ADR-0373 landed batch-2 (`float_motion_hip`), six of eleven HIP feature
extractors remained at the `-ENOSYS` scaffold posture. The batch-3 goal was to
promote two straightforward extractors — `float_moment_hip` and `float_ssim_hip`
— to real HIP module-API consumers, following the pattern established by PR #612
(`float_psnr_hip`, ADR-0254), batch-1 (ADR-0372), and batch-2 (ADR-0373).

`float_moment_hip` was the simplest remaining target: a single kernel dispatch per
frame with four uint64 atomic accumulators, directly mirroring
`cuda/integer_moment/moment_score.cu`. `float_ssim_hip` was the most
architecturally complex of the tractable targets: a two-pass separable 11-tap
Gaussian (horizontal pass into five intermediate float buffers, vertical pass +
SSIM combine + per-block float partial sum), directly mirroring
`cuda/integer_ssim/ssim_score.cu`. Both are scale=1-only in v1 (same as the CUDA
and Vulkan twins).

The remaining five extractors (`ciede_hip`, `integer_psnr_hip` — already promoted
in batch-1 — `integer_motion_v2_hip`, `adm_hip`, `vif_hip`) and the old-style
`motion_hip.c` stub (non-`VmafFeatureExtractor` API) are deferred to future
batches.

## Decision

We promote `float_moment_hip` and `float_ssim_hip` from `-ENOSYS` stubs to real
`#ifdef HAVE_HIPCC` dual-path consumers.

**`float_moment_hip`** (`hip/float_moment/moment_score.hip` + updated
`float_moment_hip.c` / `float_moment_hip.h`):

- Two kernels: `calculate_moment_hip_kernel_8bpc` / `_16bpc` (same 7-arg
  signature for both — no `bpc` arg; raw uint8 bytes addressed as uint16 for
  16bpc).
- Warp reduction: two uint32 `__shfl_down` shuffles for the 64-lane GCN/RDNA
  warp (mirrors CUDA twin's `warp_reduce_u64` technique).
- Accumulator pattern: `hipMemsetAsync` zeros the four device uint64 counters
  before each dispatch; `hipMemcpyAsync` reads them back after.
- Host: four sums divided by `w×h` → four `float_moment_*` features.

**`float_ssim_hip`** (`hip/float_ssim/ssim_score.hip` + updated `float_ssim_hip.c`
/ `float_ssim_hip.h`):

- Three kernels: `calculate_ssim_hip_horiz_8bpc`, `calculate_ssim_hip_horiz_16bpc`,
  `calculate_ssim_hip_vert_combine`.
- Warp size 64 (GCN/RDNA): `SSIM_WARPS_PER_BLOCK = 128 / 64 = 2` (vs CUDA's
  `128 / 32 = 4`). The shared-memory warp-sum array is sized accordingly.
- Five intermediate float device buffers (`d_ref_mu`, `d_cmp_mu`, `d_ref_sq`,
  `d_cmp_sq`, `d_refcmp`) allocated via `hipMalloc` (no wrapper struct, unlike the
  CUDA `VmafCudaBuffer` layer).
- Two luma staging buffers (`ref_in`, `cmp_in`) for HtoD copies
  (`hipMemcpy2DAsync`, `hipMemcpyHostToDevice`), consistent with T7-10b's
  "pictures arrive as CPU VmafPictures" posture.
- Both passes run on the same private stream; implicit stream ordering provides
  the happens-before between Pass 1 writes and Pass 2 reads.
- v1 constraint: `scale=1` only, enforced at `init()` with `-EINVAL`.
- Host: per-block float partials accumulated in `double`, divided by
  `(W-10)×(H-10)` → single `float_ssim` feature.

Both host TUs follow the `float_psnr_hip.c` `#ifdef HAVE_HIPCC` dual-path pattern:
helper functions that call HIP APIs are inside `#ifdef HAVE_HIPCC`; free-helpers
called from error labels are defined outside the guard with internal
`#ifdef HAVE_HIPCC` guards (mirrors `float_psnr_hip_module_free`). Without
`enable_hipcc`, `init()` returns `-ENOSYS` (scaffold contract preserved).

`moment_score` and `ssim_score` are added to the `hip_kernel_sources` dict in
`core/src/meson.build`, extending the `hipcc --genco` → HSACO → `xxd -i`
pipeline established by ADR-0372.

HIP real-kernel count: **6 of 11 extractors**.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Also promote `integer_motion_v2_hip` | One more extractor done | Algorithm (convolution linearity trick) requires a different kernel shape — needs a separate `.hip` file and host wiring distinct from `motion/motion_score.hip` | Deferred to batch-4 for focused review |
| Promote `ciede_hip` instead of `float_ssim_hip` | Simpler reduction (per-block float partials, single pass) | Less user-visible than SSIM; SSIM closes the more prominent quality metric gap | `float_ssim_hip` chosen for higher impact |
| Use `vmaf_hip_kernel_submit_pre_launch` to zero partials for SSIM | Uniform pre-launch contract | SSIM uses per-block writes, not atomics — no zero needed; the helper is for atomic accumulator consumers | Not used for SSIM, used correctly for moment |

## Consequences

- **Positive**: `float_moment_hip` and `float_ssim_hip` produce real scores on
  AMD GCN/RDNA hardware when built with `enable_hip=true enable_hipcc=true`.
  CPU-only and HIP-without-hipcc builds preserve the `-ENOSYS` scaffold posture
  unchanged.
- **Positive**: `moment_score.hip` and `ssim_score.hip` join the HSACO build
  pipeline; future GPU-specific work on these metrics starts from working device
  code rather than from scratch.
- **Negative**: Two new HSACO fat binaries in the build output; adds approximately
  30-60 s to `hipcc --genco` build time per target architecture.
- **Neutral**: `float_ssim_hip` v1 rejects `scale != 1` with `-EINVAL`, matching
  the CUDA and Vulkan twins. Lifting this constraint is a future batch item.
- **Neutral / follow-up**: `motion_hip.c` (old-style API, not a
  `VmafFeatureExtractor`) and the remaining five stubs (`ciede_hip`,
  `integer_motion_v2_hip`, `adm_hip`, `vif_hip`, `integer_motion_hip` — not yet
  registered) are deferred to batch-4.

## References

- ADR-0254: `float_psnr_hip` — first real HIP kernel, establishes the
  `#ifdef HAVE_HIPCC` dual-path pattern.
- ADR-0372: HIP batch-1 (`integer_psnr_hip`, `float_ansnr_hip`).
- ADR-0373: HIP batch-2 (`float_motion_hip`).
- ADR-0260: `float_moment_hip` scaffold (fourth consumer).
- ADR-0274: `float_ssim_hip` scaffold (eighth consumer).
- CUDA twins: `cuda/integer_moment/moment_score.cu`,
  `cuda/integer_ssim/ssim_score.cu`, `cuda/integer_ssim_cuda.c`.
- req: "open a NEW PR replacing 2-3 more HIP -ENOSYS stubs with real
  implementations … Pick the simplest 2-3 for batch-3: motion_hip, float_ssim_hip,
  and float_moment_hip."
