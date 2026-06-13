# HIP: promote `VMAF_FEATURE_EXTRACTOR_HIP` on `integer_motion_hip` (ADR-0530)

Extends ADR-0519. The HIP-backed `integer_motion` feature extractor
now actually dispatches to the HIP kernel
(`calculate_motion_score_kernel_8bpc`) when `--backend hip` is set,
instead of silently falling back to the CPU twin. Verified on AMD
gfx1036 (Radeon 680M) inside `vmaf-dev-mcp`:

```bash
vmaf --reference /workspace/python/test/resource/yuv/src01_hrc00_576x324.yuv \
     --distorted /workspace/python/test/resource/yuv/src01_hrc01_576x324.yuv \
     --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
     --backend hip --feature integer_motion --json --output /tmp/hip_motion.json
# VMAF = 76.7125 (CPU baseline 76.6678 — within places=4 cross-backend gate)
```

`AMD_LOG_LEVEL=3` confirms 48 HSACO kernel launches per 48-frame
clip; before this PR the HIP integer_motion path never executed any
HIP code because `compute_fex_flags()` didn't include the HIP slot
and the per-extractor flag bit was cleared.

### What ships

- New picture-buffer enum entry `VMAF_PICTURE_BUFFER_TYPE_HIP_DEVICE`
  reserved for the future HIP picture pool. Today's HIP TUs accept
  `VMAF_PICTURE_BUFFER_TYPE_HOST` and do their own HtoD copy.
- `feature_extractor.c` dispatch rejects HIP-flagged extractors
  receiving foreign GPU buffers and rejects non-HIP extractors
  receiving `HIP_DEVICE` buffers (symmetric to the existing CUDA
  check).
- `compute_fex_flags()` now adds `VMAF_FEATURE_EXTRACTOR_HIP`
  whenever a HIP state has been imported via
  `vmaf_hip_import_state()` (host-pic only, no gpumask gate —
  mirrors Vulkan).
- `vmaf_get_feature_extractor_by_feature_name()` now falls back to
  any unflagged extractor when the preferred-flag pass misses, so a
  partially-ported HIP backend still routes the missing features
  through CPU twins instead of failing model load.
- `flush_context_serial()` drains HIP-flagged extractors'
  `gpu_pending` final-frame collect (mirrors the SYCL pattern from
  `flush_context_sycl`).
- HIP integer_motion's collect / flush paths now route writes
  through `vmaf_feature_collector_append_with_dict()` so the
  encoded option-aware key matches what
  `vmaf_predict_score_at_index()` looks up.
- `vmaf_fex_integer_motion_hip` extern declaration, registration in
  the registry list, and `motion_score` HSACO entry in the meson
  `hip_kernel_sources` map (previously the source file existed but
  was not compiled or linked).

### What does NOT ship

`vmaf_fex_integer_vif_hip` was speculatively flagged in its batch-1
commit but crashes with a GPU memory access fault on the first
frame when the dispatch actually picks it. ADR-0530 un-flags it
until the kernel-level fix lands. The remaining HIP extractors
(psnr / ciede / float_moment / float_ansnr / motion_v2 /
float_motion / float_ssim / float_psnr / cambi / float_adm /
ssimulacra2) stay unflagged pending per-extractor end-to-end
verification.

### ADRs

- [ADR-0530](../docs/adr/0530-hip-feature-flag-promotion-and-picture-buffer.md)
  — this change.
- [ADR-0519](../docs/adr/0519-hip-import-state-implementation.md) —
  the parent (made the HIP backend load end-to-end).
- [ADR-0468](../docs/adr/0468-hip-integer-motion-port.md) — the
  underlying `integer_motion_hip` kernel port.
