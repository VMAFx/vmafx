# ADR-1183: Model options gate GPU twin selection

- **Status:** Accepted
- **Date:** 2026-09-05
- **Deciders:** Lusoris
- **Supersedes:** none
- **Superseded by:** none

## Context

In `core/src/feature/feature_extractor.cpp`, option parsing for feature
extractors (`vmaf_fex_ctx_parse_options`) previously iterated solely over the
target feature extractor's own declared option table, matching each against
keys present in the provided `opts_dict`. Any key present in `opts_dict` that
was not recognized by the selected extractor was silently ignored.

When the fork transitioned its default model to `vmaf_v1.0.16_3d0h`
([ADR-1169](1169-default-model-v1-0-16.md)), this silent omission surfaced a
critical defect. The `vmaf_v1.0.16_3d0h` model sets `adm_csf_mode: 2` (along
with `adm_dlm_weight: 0.7`, `adm_norm_view_dist: 3.0`, etc.) and
`motion_max_val: 18`. On `--backend cuda` (and similarly SYCL and HIP), the GPU
twin `integer_adm_cuda` only implements CSF mode 0 (Watson97) and lacks
`adm_csf_mode` (as well as `adm_p_norm`). Under the prior behavior:

1. The GPU twin `integer_adm_cuda` was selected by
   `vmaf_use_features_from_model`.
2. The `adm_csf_mode` option was silently dropped during option parsing.
3. ADM was computed with the wrong CSF mode, and the emitted feature name was
   `integer_adm3_dlmw_...` rather than `integer_adm3_csf_2_dlmw_...`.
4. The model execution failed with missing feature keys (`problem generating
   pooled VMAF score`) or would have produced silently corrupted scores.

A second defect existed on the CLI: user typos in `--feature` option strings
(e.g., `--feature adm=adm_csf_moed=2`) were silently ignored, causing commands
to run with unexpected default parameters without notice.

## Decision

1. **Reject unknown feature options:** `vmaf_fex_ctx_parse_options` now
   iterates over all keys provided in `opts_dict`. If any key does not match
   an option name or alias recognized by the selected feature extractor,
   libvmaf logs an error at `VMAF_LOG_LEVEL_ERROR`:
   `feature extractor '<name>': unknown option '<key>'` and returns `-EINVAL`.
2. **Gate GPU twin selection by model options:** In `core/src/libvmaf.c`
   (`vmaf_use_features_from_model`), before selecting a GPU-accelerated twin
   returned by `vmaf_get_feature_extractor_by_feature_name`, libvmaf verifies
   that the GPU twin supports every option key requested in the model's feature
   options dictionary. If the GPU twin lacks any option requested by the
   model:
   - libvmaf logs an informational notice at `VMAF_LOG_LEVEL_INFO`:
     `<backend> extractor lacks option '<key>', computing it on the CPU`
   - libvmaf falls back to selecting the CPU twin (`flags = 0`) for that
     specific feature.
   If the CPU twin also lacks the option, the error from step 1 cleanly fires
   during context initialization.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Silent ignore (status quo) | Simple; GPU twins run whatever subset of options they recognize. | Produces incorrect scores, mismatched feature dictionary names, and silently swallows user typos. | Rejected: correctness is non-negotiable. |
| Hard error with no CPU dispatch | Simple to implement; immediately exposes gaps. | Breaks execution of `vmaf_v1.0.16_3d0h` on GPU backends where individual options like `adm_csf_mode` are not yet ported. | Rejected: the default model must run reliably across all backends. |
| Block release until all GPU twins port missing options | GPU acceleration for all features immediately. | Porting `adm_csf_mode` (and related options) across CUDA, SYCL, and HIP requires extensive kernel development and validation. | Rejected: delays 1.0.0 release. Tracked as an open backlog item (`T-GPU-ADM-CSF-MODE-NOT-PORTED-2026-09-05`). |

## Consequences

- **Positive:** The default model `vmaf_v1.0.16_3d0h` executes successfully
  and reliably on CUDA, SYCL, and HIP. Features whose options are fully
  supported on the GPU (such as CAMBI, SpEED) run on device, while unsupported
  feature configurations (like `adm3` with CSF mode 2) run on the CPU reference
  with bit-exact numerical parity to the CPU pipeline.
- **Positive:** Feature option typos on CLI or API fail immediately with an
  explicit error naming the unknown option.
- **Negative:** Feature extractors dispatched to the CPU do not benefit from
  GPU acceleration until their option support is implemented on the GPU
  backend.
- **Neutral / follow-ups:** Track porting of `adm_csf_mode` to CUDA, SYCL, and
  HIP in `docs/state.md` under `T-GPU-ADM-CSF-MODE-NOT-PORTED-2026-09-05`.

## References

- Brief `brief-cambicuda2.md` requirements 2, 3, 6
- [ADR-1169](1169-default-model-v1-0-16.md): Default model `vmaf_v1.0.16_3d0h`
- [ADR-0165](0165-state-md-bug-tracking.md): State tracking
