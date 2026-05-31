<!-- markdownlint-disable MD060 -->
# ADR-0537: HIP integer VIF kernel crash fix — filter upload, bounds, HtoD staging

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `hip`, `gpu`, `kernel`, `vif`, `bug-fix`

## Context

ADR-0530 (PR #1287) wired `compute_fex_flags()` to add
`VMAF_FEATURE_EXTRACTOR_HIP` whenever a HIP state is imported, so the
model-driven dispatch finally picks HIP-flagged extractors instead of
falling through to the CPU twins.  As a known consequence
ADR-0530 explicitly left `vmaf_fex_integer_vif_hip` un-flagged with an
inline citation: "speculatively flagged in batch-1 (ADR-0254) but
crashes with a GPU memory access fault on the first frame".  This ADR
closes that follow-up.

Investigation found four separate defects in the integer-VIF HIP path:

1. **Filter table passed as host pointer.** The kernel signature was
   `const uint16_t filter[18]` (a pointer parameter); the host launch
   args contained `(void *)vif_filter1d_table[0]` — the *address of a
   host-side static array*.  HIP's `hipModuleLaunchKernel` happily
   copied that 8-byte value into the kernel argument region and the
   GPU dereferenced it on the first load, faulting immediately with
   "Memory access fault by GPU node-1 ... Reason: Page not present".

2. **Wrong filter half-widths.** The kernel hard-coded `HALF = 9`
   (etc.) from the second number in the kernel-name suffix
   (`filter1d_8_vertical_kernel_uint32_t_17_9` → `HALF=9`).  The
   suffix actually encodes `fwidth_0 = 17` and `fwidth_1 = 9` —
   *two distinct widths*, not a half-width.  The correct
   half-widths are `vif_filter1d_width[scale] / 2 = {8, 4, 2, 1}`.
   The pre-fix kernel read 19 filter coefficients (`k = -9..9`) from
   an 18-entry table, reading one past the end every scale-0 launch.

3. **No downsample write for next scale.** The CUDA twin's
   scale-0/1/2 kernels compute the `*_convol` rd-filter residual and
   write half-resolution ref / dis planes into `buf.ref` / `buf.dis`.
   The pre-fix HIP kernel skipped this step entirely, so scales 1–3
   read uninitialised device memory as their input frame.  Even with
   the host pointer faulted around, the resulting numerics would
   have been garbage.

4. **Picture buffer reads from host memory.** The HIP picture pool
   isn't wired yet (per the ADR-0530 docblock — "HIP TUs still accept
   HOST buffers and do their own HtoD copy").  The pre-fix submit
   passed `ref_pic->data[0]` (a host pointer) directly to the
   kernel.  Same fault class as (1), separately reachable once (1)
   was fixed.

After PR #1287 promoted the dispatch flag bit, ADR-0530 immediately
un-flagged the VIF extractor with an inline citation pointing to this
follow-up — the fix wasn't ready to ship in the same PR.  This ADR
records the closing of that follow-up.

## Decision

Rewrite `core/src/feature/hip/integer_vif/vif_statistics.hip` and
`core/src/feature/hip/integer_vif_hip.c` to correct all four
defects, then re-enable `VMAF_FEATURE_EXTRACTOR_HIP` on
`vmaf_fex_integer_vif_hip`:

1. **Filter upload.** Allocate a 144-byte device buffer at `init()`
   and `hipMemcpy` the full `vif_filter1d_table[4][18]` into it
   once per extractor lifetime.  Every kernel launch now passes
   `&s->vif_filt_dev` (address of the variable storing the device
   pointer) so the kernel receives a valid device pointer.

2. **Correct half-widths.** Use `vif_filter1d_width[scale] / 2 =
   {8, 4, 2, 1}` inside the kernel, expressed as `#define VIF_FW_S0
   17 / VIF_FW_S1 9 / VIF_FW_S2 5 / VIF_FW_S3 3` with
   `HALF = FW / 2` derivation.

3. **Downsample write path.** Each scale-0/1/2 horizontal kernel
   now applies the `FW_RD`-tap rd filter to `tmp.ref_convol /
   tmp.dis_convol` and writes the decimated-by-2 half-resolution
   `uint16_t` planes to `buf.ref / buf.dis`, matching the CPU
   `subsample_rd_*` + `decimate_and_pad` reference paths.

4. **HtoD picture staging.** Allocate per-frame device staging
   buffers `s->ref_in_dev` / `s->dis_in_dev` at init; before each
   scale-0 launch, `hipMemcpy2DAsync` copies the host Y plane into
   them (mirrors the integer_motion_hip.c pattern at lines 372–373).
   Scales 1–3 read the half-res rd-buffers written by the previous
   horizontal pass.

The kernel is intentionally written as a *scalar-per-thread*
implementation: it drops the CUDA twin's shared-memory tiling and
warp-reduce micro-optimisations in favour of a small, verifiable
surface (~540 lines vs. ~850 in the CUDA twin).  Correctness first;
perf optimisation is deferred to a follow-up ADR once the cross-
backend parity gate confirms the baseline is stable.

### Adjacent fixes bundled into this PR

The same PR also closes three small adjacent gaps surfaced by the
`enable_hipcc=true` build:

- **Missing HSACO entries in `hip_kernel_sources`.** ADR-0533
  (PR #1292) wired the HIP-extractor registration sweep but did not
  add the corresponding `_hsaco` blobs to the meson dict.  Seven
  consumers (`motion_score`, `ms_ssim_score`, `psnr_hvs_score`,
  `integer_ssim_score`, `float_vif_score`, `ssimulacra2_blur`,
  `ssimulacra2_mul`) referenced `<name>_hsaco` symbols under
  `#ifdef HAVE_HIPCC` that the build never compiled.  These are
  added.

- **ADM kernels carry CUDA-helper dependencies.** Four ADM .hip
  files (`adm_dwt2`, `adm_csf`, `adm_csf_den`, `adm_cm`) reference
  CUDA-specific types (`uint64_cu`, `cta_size`, `warp_reduce`,
  `VMAF_CUDA_THREADS_PER_WARP`) that don't compile under
  `hipcc --genco`'s minimal include set.  A weak-stub TU
  (`hip_hsaco_stubs.c`) provides empty fallback blobs for those
  four symbols so the link succeeds; the host `hipModuleLoadData`
  call against the empty blob returns an error and the extractor
  falls back to CPU through the ADR-0530 dispatch fallback.  When
  any individual ADM kernel is ported off the CUDA helpers, adding
  it to `hip_kernel_sources` will override the weak symbol at link
  time.

- **HIPcc include path missing `build_dir` + `feature/hip` + `hip`.**
  The `hipcc --genco` custom_target only had `meson.current_source_dir()`
  `.../feature` in the include list, so kernels including
  `hip/integer_*_hip.h` (which transitively pull in `config.h` from
  the build directory) failed to find headers.  Three more `-I`
  entries fix it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Port the CUDA twin verbatim (templates + shared-memory + warp-reduce) | Best perf; closer parity story | ~3× more code; HIP wavefront-size differences (RDNA2=32, gfx9XX=64) need separate tuning; risk of subtle numerical divergence in the warp-reduce path | Scalar-per-thread is sufficient correctness baseline; perf optimisation is its own ADR once the parity gate is stable |
| Pass the filter table as a by-value struct (CUDA pattern) | Avoids the device-pointer indirection | HIP module-launch struct-by-value works but adds an aliasing surface for the kernel's `(const uint32_t *)&mu1` reinterpret pattern; the 144-byte upload is one-shot at init so the perf cost is amortised | Device-pointer pattern is the more portable HIP idiom and matches the integer_motion_hip / float_psnr_hip precedent |
| Keep the flag cleared and defer the kernel fix | Smallest delta; lowest risk of new regression | Leaves the model-driven `--backend hip` path inconsistent (some flagged, some cleared); blocks the cross-backend parity gate from running VIF on HIP | The flag flip is the entire point of ADR-0530's follow-up; deferring it leaves the gap permanently |

## Consequences

**Positive:**

- `vmaf --backend hip --feature vif_hip` no longer crashes on
  gfx1036; produces VMAF scores end-to-end on the Netflix golden
  pair (src01_hrc00 vs src01_hrc01 at 576×324).
- The HIP dispatch path is now consistent: every HIP-registered
  extractor whose kernel works has `VMAF_FEATURE_EXTRACTOR_HIP`
  set; the ADR-0530 fallback handles unflagged misses.
- The `enable_hipcc=true` build path links cleanly under
  ADR-0533's extractor-registration sweep.

**Negative:**

- HIP VIF scores are within ~0.003 of CPU per scale on the Netflix
  golden pair (places=3 — not places=4 of ADR-0214).  Sources of
  divergence: clamp boundary inside the kernel vs CPU's pre-padded
  mirror buffer; cumulative downsampling error compounding across
  scales.  Cross-backend parity gate currently passes at places=3
  for VIF on HIP; a follow-up ADR will tighten to places=4 by
  porting the CPU's pre-pad boundary semantics into the kernel.
- The scalar-per-thread kernel is correctness-first, not perf-first.
  Throughput will be ~5–10× slower than the CUDA twin on
  equivalent silicon.  Acceptable for now (HIP path is
  developer-only); the perf optimisation is a separate ADR.

**Neutral / follow-ups:**

- Tighten cross-backend parity from places=3 to places=4 for HIP
  VIF by porting the CPU boundary semantics.
- Port the CUDA twin's shared-memory tiling for perf parity.
- Port the four ADM .hip kernels off the CUDA helper macros so
  the weak HSACO stubs can be removed.
- Real HIP picture pool (so HtoD staging in submit can be
  eliminated).

## References

- `req`: "Fix the `vmaf_fex_integer_vif_hip` GPU memory access fault."
- ADR-0530: HIP feature-flag promotion + HIP_DEVICE picture-buffer type.
- ADR-0533: HIP-extractor registration sweep (PR #1292).
- ADR-0254: Original HIP feature-flag scaffold (batch-1).
- ADR-0214: Cross-backend parity gate (places=4).
- ADR-0468: integer_motion_v2_hip scaffold (HtoD staging precedent).
- AMD_LOG_LEVEL=3 trace confirming kernel launches succeed:
  `/tmp/amd.log` in the PR description.
