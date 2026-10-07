---
paths:
  - core/src/feature/metal/*.mm
  - core/src/feature/metal/float_ms_ssim_option_semantics.h
invariant: A Metal twin mirrors CPU option table and emits CPU outputs; names before option slots.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Per-feature option-table sync invariant

- **`float_ms_ssim_metal` exposes full option and score parity.** ADR-1334
  extends ADR-1221 to resolve `T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07`:
  `float_ms_ssim_metal` exposes `enable_lcs`, `enable_db`, `clip_db`, and `enable_chroma`.
  Scores are emitted via shared `vmaf_ms_ssim_emit_scores()` (for luma) and
  `vmaf_ssim_emit_score_named()` (for chroma planes `float_ms_ssim_cb` and
  `float_ms_ssim_cr`), passing `s->enable_db, s->max_db`. When `clip_db` is enabled,
  `s->max_db` is derived from frame geometry via
  `ceil(10. * log10(peak * peak / mse))` with `mse = 0.5 / (w * h)`.
  framework-free `float_ms_ssim_option_semantics.h` owns active-plane count,
  ceil-subsampled plane geometry, and dB ceiling so exact production
  semantics execute on hosts without Metal.
  Subsampled chroma requires at least 176x176 dimensions (351x351 luma for
  YUV420P because allocation uses ceil subsampling), enforced at init. YUV400P
  resolves to one active plane before that chroma check. Every L/C/S atom on
  every active plane is validated before weighted product;
  `pow(NaN, 0)` must never erase failed reduction.
  `test_metal_ms_ssim_option_semantics`, `test_metal_ms_ssim_options_contract.py`,
  and `test_nonfinite_collector_wiring.py` lock this contract down device-free.
  CUDA and HIP twins include same header for their `enable_chroma`
  plane count and plane sizes (`T-MS-SSIM-GPU-CHROMA-OPTION-DRIFT-2026-09-06`):
  rename or signature change of its helpers changes
  `../cuda/integer_ms_ssim_cuda.c` and `../hip/integer_ms_ssim_hip.c` in
  same PR.
- **GPU twins must mirror CPU option table for model-configured
  features.** Model (such as default model `vmaf_v1.0.16_3d0h`) may
  provide feature options. `vmaf_use_features_from_model` then checks
  that selected backend's feature extractor supports every option
  present in model's options dictionary. If any option missing from
  GPU twin's `options[]`
  table, dispatch rejects twin and falls back to CPU. For CAMBI
  (`integer_cambi_metal`), `cambi_high_res_speedup` (alias `hrs`,
  default 0, min 0, max 2160) must be present in option table and
  honored during processing (adjusting window size and subsampling
  post-spatial-mask for resolutions >= 1080p, matching `cambi.c`).
- **Never re-declare shared CAMBI constants.** Resolution thresholds
  (`CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p` / `_1440p` / `_2160p`) and
  `CAMBI_WINDOW_DIVISOR` live in `core/src/feature/cambi_internal.h`,
  which `integer_cambi_metal.mm` already includes. Metal-local copy
  of shared constant is how twin silently drifts from `cambi.c` on
  next upstream sync.
- **`submit_fex_metal` must leave `s->d_image` / `s->d_mask` /
  `s->d_tmp` pointing at allocations `init_fex_metal` made.** Per-scale
  pipeline rotates three scratch buffers via pointer swaps; every exit
  path restores originals so next frame starts from known state and
  `close_fex_metal` releases handles it owns.
- **`vmaf_use_feature()` consumes option dictionary.** It takes
  ownership of `VmafFeatureDictionary` on every path except
  argument-validation guards, so CPU-vs-Metal parity test must build
  fresh dictionary per call. Sharing one across two runners =
  use-after-free; freeing it afterwards = double free.
