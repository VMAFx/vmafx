# ADR-0202: float_adm CUDA + SYCL twins — sixth Group B float kernel finishes

- **Status**: Accepted
- **Date**: 2026-04-27
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: cuda, sycl, gpu, feature-extractor, fork-local, places-4

## Context

[ADR-0192](0192-gpu-long-tail-batch-3.md) tracks the GPU long-tail
batch 3 roadmap; [ADR-0199](0199-float-adm-vulkan.md) shipped the
Vulkan kernel for `float_adm` as part 6a. This ADR closes parts 6b
(CUDA) and 6c (SYCL) — the cross-backend twins of the same kernel,
following the established two-PR pattern from earlier batches
(motion_v2 #146 → #147, psnr_hvs #143 → #144, etc.).

CPU reference:
[`float_adm.c`](../../core/src/feature/float_adm.c) (thin wrapper)

- [`adm.c::compute_adm`](../../core/src/feature/adm.c) (4-scale
orchestration) +
[`adm_tools.c`](../../core/src/feature/adm_tools.c) (the float
`_s`-suffixed primitives). Vulkan structural reference:
[`float_adm_vulkan.c`](../../core/src/feature/vulkan/float_adm_vulkan.c)
and
[`float_adm.comp`](../../core/src/feature/vulkan/shaders/float_adm.comp).

## Decision

Ship `float_adm_cuda` and `float_adm_sycl` as direct ports of the
Vulkan kernel — same four pipeline stages, same `-1` mirror form on
both axes, same fused stage 3 with cross-band CM threshold. The CPU
extractor's `places=4` precision contract is preserved by both
backends.

### CUDA twin —

[`float_adm_cuda.c`](../../core/src/feature/cuda/float_adm_cuda.c)
([`float_adm_score.cu`](../../core/src/feature/cuda/float_adm/float_adm_score.cu))

- Single `.cu` file with four `__global__` entry points (one per
  stage), same shape as the float_vif CUDA kernel from
  [ADR-0197](0197-float-vif-gpu.md). Submit/collect async-stream
  pattern matches `motion_cuda` and `float_vif_cuda`.
- Per-frame: 16 launches (4 stages × 4 scales) on the picture
  stream + a D2H copy of per-scale (csf, cm) partials on the
  secondary event stream. Reduction across WGs runs on the host
  in `double`.
- Stage 3 grid: `(3 × num_active_rows, 1, 1)` workgroups, each
  with a `WG_SIZE = 16 × 16` block. Warp + cross-warp reduction
  via `__shfl_down_sync` mirrors the Vulkan `subgroupAdd` tree.
- Per-scale `ref_band[]` / `dis_band[]` allocations (one buffer
  per scale) so the next scale's stage 0 can read the parent
  LL band without overwriting it. Vulkan reuses one buffer
  serially; CUDA pays a tiny extra-allocation cost for clearer
  ownership and easier debugging.

### SYCL twin —

[`float_adm_sycl.cpp`](../../core/src/feature/sycl/float_adm_sycl.cpp)

- Single `.cpp` file with four `launch_*` templates over `SCALE`,
  same shape as `float_vif_sycl.cpp`. Self-contained
  submit/collect — does NOT register with the shared_frame
  preallocation model (the multi-scale band/csf layout doesn't
  fit, same rationale as float_vif).
- `[[intel::reqd_sub_group_size(32)]]` on the stage-3 reduction
  ensures portable warp behaviour across Intel + Nvidia SYCL
  back-ends.

### `-fmad=false` for the CUDA fatbin

The Vulkan kernel uses the GLSL `precise` qualifier on the
angle-flag dot product (`ot_dp = oh*th + ov*tv`) so the comparison
`lhs >= rhs` does NOT depend on FMA contraction. NVCC's default
`-fmad=true` fuses the same expression into FMA(ov, tv, oh*th),
which cascades through the CSF / CM cube reductions and pushes
scale-3 and adm2 past `places=4` (max_abs_diff seen at 3.6e-4 on
the Netflix normal pair before fixing).

`meson.build` now carries a small per-kernel flag dict
(`cuda_cu_extra_flags`) and threads `--fmad=false`

- `-Xcompiler=-ffp-contract=off` into the `float_adm_score`
fatbin only — the integer ADM kernel uses `int64` accumulators
for which FMA is irrelevant, so the existing FMA-on path is
preserved for it. **This is a precision contract guard, not a
performance regression** — `-fmad=false` only affects the four
`float_adm_*` kernels and leaves the rest of the CUDA build
untouched.

### Parent-LL dimension trap (load-bearing)

Stage 0 at `scale > 0` reads the parent's LL band. The
mirror/clamp clamp dimensions are the **parent's LL output
dimensions** (= `scale_w/h[scale]`, the input dims at the current
scale), NOT the parent's full-resolution image dimensions
(= `scale_w/h[scale - 1]`). The Vulkan kernel passes
`pc.cur_w/cur_h` which match the former; the first cut of the
CUDA + SYCL submit code passed `scale_w/h[scale - 1]`, which
clamped against the wrong bounds and let the parent reads
wander into uninitialised memory at scale 1+. Symptom:
`max_abs_diff = 3.6e-4` at `adm_scale3` and `1.4e-4` at `adm2`
on the Netflix normal pair. Fix:
[`float_adm_cuda.c::submit_fex_cuda`](../../core/src/feature/cuda/float_adm_cuda.c)

- [`float_adm_sycl.cpp::submit_fex_sycl`](../../core/src/feature/sycl/float_adm_sycl.cpp)
both now pass `scale_w/h[scale]`. Cited inline at the
declaration so future refactors don't regress the bounds.

## Alternatives considered

1. **Five-file split per integer ADM CUDA pattern**
   (`adm_dwt2.cu`, `adm_decouple.cu`, `adm_csf.cu`,
   `adm_csf_den.cu`, `adm_cm.cu`). Reasoning: matches the most
   structurally-similar precedent. Rejected because the float
   pipeline shares no header dependencies with the integer
   path, the float_vif precedent uses one file
   (`float_vif/float_vif_score.cu`), and meson's per-target
   compile flags machinery is simpler with a single-file
   target. The user instructions called the directory layout
   "you can reuse" — i.e. permissive, not mandatory.
2. **Targeted `__fmul_rn` / `__fadd_rn` intrinsics** in the
   angle-flag and cube reductions instead of a TU-wide
   `-fmad=false`. Rejected because the affected expressions
   appear in five distinct device functions and each rewrite
   would need a paired comment chain explaining "this exact
   parens layout matters"; the per-kernel meson flag is one
   line and isolates the fix to the kernel that needs it.
3. **Bundle this PR with the ssimulacra2 cuda/sycl twins**
   (the only other batch-3 metric that hasn't fanned out).
   Rejected to keep the PR shape consistent with #144 / #147 /
   #150 / #151 — one pure-Vulkan PR, then one CUDA+SYCL twin
   PR per metric.

## Consequences

### Positive

- `float_adm` is now available on Vulkan + CUDA + SYCL (and CPU,
  AVX2, AVX-512, NEON), closing the sixth and final Group B float
  metric gap from ADR-0192.
- All five output metrics (`adm2`, `adm_scale0..3`) hit
  `max_abs_diff ≤ 6e-6` on the Netflix normal pair — the same
  tolerance the Vulkan kernel achieves, and well inside the
  `places=4` contract.

### Negative

- One new fatbin (`float_adm_score`) compiled with
  `--fmad=false`. Affects only this kernel; the rest of the
  CUDA build keeps its default FMA behaviour.
- The Vulkan host wrapper allocates one band buffer; CUDA
  allocates four (one per scale). Per-frame device memory
  delta: ~`4 × buf_stride × half_h0 × float = 4 × 288 × 162 ×
  4 = ~720 KiB` per CUDA frame on the Netflix normal pair.

## Reproducer

```bash
# CUDA + SYCL build inside libvmaf/.
PATH="/opt/intel/oneapi/compiler/latest/bin:/opt/cuda/bin:$PATH" \
CXX=/opt/intel/oneapi/compiler/latest/bin/icpx \
CC=/opt/intel/oneapi/compiler/latest/bin/icx \
  meson setup core/build_cs --reconfigure \
    -Denable_cuda=true -Denable_sycl=true -Denable_vulkan=enabled \
    -Denable_float=true \
    -Dsycl_compiler=/opt/intel/oneapi/compiler/latest/bin/icpx \
    libvmaf
ninja -C core/build_cs

# Cross-backend gate, places=4.
python3 scripts/ci/cross_backend_vif_diff.py \
  --vmaf-binary core/build_cs/tools/vmaf \
  --reference python/test/resource/yuv/src01_hrc00_576x324.yuv \
  --distorted python/test/resource/yuv/src01_hrc01_576x324.yuv \
  --width 576 --height 324 --feature float_adm \
  --backend cuda --places 4
# Expected: 0/48 mismatches across all 5 metrics, max_abs_diff ≤ 6e-6.
```

## References

- `req` — user task instruction: "Implement `float_adm_cuda` +
  `float_adm_sycl` — the CUDA and SYCL GPU twins of the just-shipped
  `float_adm_vulkan` kernel".
- [ADR-0192](0192-gpu-long-tail-batch-3.md) — batch-3 roadmap.
- [ADR-0199](0199-float-adm-vulkan.md) — Vulkan kernel parent.
- [ADR-0197](0197-float-vif-gpu.md) — closest Group B float twin
  precedent (CUDA + SYCL pattern, fmad-off, mirror-trap notes).
- [ADR-0178](0178-integer-adm-vulkan.md) — integer ADM Vulkan
  parent (algorithm shape, dispatch grid).

### Status update 2026-05-08: SYCL DWT rewrite to group_load

Per [ADR-0028](0028-adr-maintenance-rule.md) the body above is
frozen. This appendix records a downstream investigation outcome
that touches the same SYCL TU.

[Research-0086 §A.4](../research/0086-sycl-toolchain-audit-2026-05-08.md)
emitted a GO recommendation to rewrite the ADM DWT vertical and
horizontal passes in
[`integer_adm_sycl.cpp`](../../core/src/feature/sycl/integer_adm_sycl.cpp)
on top of `sycl::ext::oneapi::experimental::group_load`. The rewrite
was attempted on 2026-05-08 and **deferred** under
[ADR-0332](0332-sycl-adm-dwt-group-load-deferral.md). Two blockers
forced the deferral:

1. The vertical-pass tile (`TILE_H × WG_X = 18 × 32 = 576` int32
   elements, `WG_SIZE = 256` work-items) does not satisfy the SYCL
   ext contract `total = WG_SIZE × ElementsPerWorkItem` for any
   integer `ElementsPerWorkItem`. The general expression
   `2 × (WG_Y + 1) / WG_Y` is integer only for `WG_Y ∈ {1, 2}`,
   neither viable for the current 8-row output stride.
2. `group_load` requires a contiguous `InputIteratorT`; the
   multi-row tile load is contiguous only within a single tile row
   (`WG_X = 32` ints), separated by full `in_stride` between rows.

The horizontal pass at line 358 carries no SLM tile and was a
non-target.

The Battlemage register-pressure delta that motivated the digest's
GO recommendation is unverifiable on the dev host (Arc A380
Alchemist; no Xe2 available). The kernel remains bit-exact-untouched
on this dimension; the cross-backend gate
(`scripts/ci/cross_backend_vif_diff.py --feature adm --backend sycl`,
`places=4`) continues to apply against the unchanged manual
cooperative tile load. See [ADR-0332](0332-sycl-adm-dwt-group-load-deferral.md)
for the full alternatives matrix and re-open conditions.
